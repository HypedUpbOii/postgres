# Files to Add

All new code lives in `contrib/auto_index/` as a standard PostgreSQL extension.
No postgres source files are modified — the extension loads via `shared_preload_libraries`.

---

## Implementation Order

Build and verify each layer before moving to the next:

1. **Scaffold** — create all files, get the extension compiling and loading cleanly
   (`shared_preload_libraries = 'auto_index'`, restart, check logs for errors)
2. **Shared memory** — implement `shmem_request_hook` and `shmem_startup_hook`,
   verify with a stub SQL function that reads a value from the segment
3. **Write tracking** — extend `ExecutorEnd_hook` to increment insert/update/delete
   counters by inspecting `queryDesc->operation`; test by running INSERTs and querying
   the stats function
4. **Read tracking** — extend `ExecutorEnd_hook` to walk the plan tree and record
   column predicate hits from SeqScan quals; test with `SELECT ... WHERE` on an
   unindexed table
5. **Background worker** — get it waking on a latch and logging the stats snapshot;
   verify numbers look correct before adding any DDL
6. **Index decisions** — add the benefit/cost calculation and wire up
   `CREATE INDEX CONCURRENTLY` via SPI; use `pg_stat_user_indexes.idx_scan` for
   drop decisions

---

## `contrib/auto_index/auto_index.h`

Defines the shared memory layout used by every backend and the background worker.

Key structs:
- `AutoIndexColumnStats` — per-column counters: `equality_hits`, `range_hits`.
  These are only incremented when the column appears as a predicate on a `SeqScan`
  node, so they directly encode "this column was filtered during a sequential scan."
- `AutoIndexTableStats` — per-table entry: `relid` (table OID, 0 = empty slot);
  `last_access` (LRU timestamp, see below); write counters `insert_count`,
  `update_count`, `delete_count`; and an array of `AutoIndexColumnStats` keyed by
  attribute number (sized by `auto_index.max_columns_per_table`).
- `AutoIndexSharedState` — top-level segment: an `LWLock` for mutual exclusion;
  a monotonic `access_counter` (uint64) for LRU eviction ordering; a fixed-size
  array of `AutoIndexTableStats` entries (sized by `auto_index.max_tracked_tables`
  at startup); and a generation counter so the bgworker can detect resets.

### Why no seq_scan_count or index_scan_count

`equality_hits[C]` and `range_hits[C]` already encode "column C was filtered
during a seq scan" — they are only incremented inside `SeqScan` node processing.
`seq_scan_count` per table is a coarser version of the same signal and is redundant
for index candidate selection.

For DROP decisions, `pg_stat_user_indexes.idx_scan` (queried by the bgworker via
SPI) gives per-index usage counts that PostgreSQL already maintains — no need to
duplicate that tracking in shared memory.

---

## `contrib/auto_index/auto_index.c`

Main extension file. Implements all hooks, the background worker, and the index
decision engine.

### `_PG_init()`
Registers GUC parameters (`auto_index.threshold`, `auto_index.check_interval`,
`auto_index.max_indexes_per_table`), chains onto `shmem_request_hook`,
`shmem_startup_hook`, and `ExecutorEnd_hook`, and registers the background worker
with `RegisterBackgroundWorker()`.

`ProcessUtility_hook` is not used — DML statements (INSERT, UPDATE, DELETE) go
through the executor, not the utility path. Write tracking is handled entirely
inside `ExecutorEnd_hook` by inspecting `queryDesc->operation`.

### `auto_index_shmem_request()`
Called during postmaster startup (before `fork()`), before the shared memory segment
is allocated — this is the only window to reserve space.
Calls `RequestAddinShmemSpace(compute_shmem_size())` and
`RequestNamedLWLockTranche("auto_index", 1)` to reserve space for one tranche lock.

`compute_shmem_size()` reads `auto_index.max_tracked_tables` and
`auto_index.max_columns_per_table` at this point to size the allocation.
**These GUCs cannot be changed without a full server restart** — they affect shared
memory layout and are read before any config reload can fire, just like `shared_buffers`
or `max_connections`.

### `auto_index_shmem_startup()`
Called after shared memory is mapped.
Uses `ShmemInitStruct("auto_index", ...)` to attach to (or create) the segment,
then zero-initialises `AutoIndexSharedState` and stores the `LWLock` pointer from the
named tranche. Chains to any previous `shmem_startup_hook`.

### `find_or_alloc_slot(Oid relid)` — LRU slot management

Always called under the exclusive LWLock. Scans the fixed array once, tracking
three things simultaneously:
- a slot whose `relid` matches (cache hit)
- the first empty slot (`relid == 0`)
- the slot with the smallest `last_access` value (LRU victim)

On a cache hit, refreshes `last_access` and returns the index.
On a miss, uses the first empty slot if available; otherwise evicts the LRU slot
by zeroing it and writing the new `relid`. In both cases `last_access` is set to
`++auto_index_state->access_counter` — a monotonic tick that gives pure relative
ordering without a system call.

```c
static int
find_or_alloc_slot(Oid relid)
{
    int    empty_slot = -1;
    int    lru_slot   = -1;
    uint64 lru_time   = UINT64_MAX;

    for (int i = 0; i < auto_index_max_tracked_tables; i++)
    {
        AutoIndexTableStats *e = &auto_index_state->tables[i];

        if (e->relid == relid)
        {
            e->last_access = ++auto_index_state->access_counter;
            return i;
        }
        if (e->relid == 0 && empty_slot == -1)
            empty_slot = i;
        if (e->last_access < lru_time)
        {
            lru_time = e->last_access;
            lru_slot = i;
        }
    }

    int slot = (empty_slot != -1) ? empty_slot : lru_slot;
    memset(&auto_index_state->tables[slot], 0, sizeof(AutoIndexTableStats));
    auto_index_state->tables[slot].relid       = relid;
    auto_index_state->tables[slot].last_access = ++auto_index_state->access_counter;
    return slot;
}
```

Evicted stats are silently discarded — if a table was LRU it hasn't been recently
accessed so its counters are stale anyway.

### `auto_index_executor_end(QueryDesc *queryDesc)`

Fires after every query completes. Handles both write tracking and read tracking
in a single hook. Chains to `prev_ExecutorEnd`.

**Write tracking** — checks `queryDesc->operation`:
- For `CMD_INSERT`, `CMD_UPDATE`, `CMD_DELETE`: resolves the target relation OID
  from `queryDesc->plannedstmt->resultRelations`, acquires the LWLock exclusively,
  calls `find_or_alloc_slot()`, and increments the appropriate write counter.

**Read tracking** — walks `queryDesc->planstate` recursively looking for `SeqScan`
nodes only:
- For each `SeqScan` node found, acquires the LWLock exclusively and calls
  `find_or_alloc_slot()` for that table. Then inspects the `qual` list on the
  static plan node for `OpExpr` nodes of the form `Var op Const`. For each match,
  increments `equality_hits` or `range_hits` on the appropriate column slot.
- `IndexScan` nodes are not inspected — index usage tracking is delegated to
  `pg_stat_user_indexes.idx_scan` which PostgreSQL already maintains.

**Note:** Extracting attribute numbers from qual expressions is the most complex
part of the implementation. Walk the `Expr` tree looking for `OpExpr` nodes where
one argument is a `Var` and the other is a `Const`. Use `nodeSeqscan.c` and
`ExecSeqScan` as a reference for how quals are structured on the plan node.
To distinguish equality from range predicates, compare the operator OID against
known equality/inequality families via `get_op_opfamily_membership` in
`utils/lsyscache.h`.

### `auto_index_main()` — background worker entry point

Loops forever:
1. Calls `WaitLatch` with the configured check interval.
2. Acquires the LWLock in shared mode, copies the stats snapshot, releases the lock.
3. For each tracked table entry in the snapshot, calls `evaluate_index_candidates()`:
   - **CREATE path** — for each column with non-zero `equality_hits` or `range_hits`:
     - Fetches `pg_class.reltuples` via SPI for table cardinality.
     - Reads `pg_statistic` (n_distinct, correlation) for the column to estimate
       selectivity and index scan cost.
     - Computes `benefit = (equality_hits + range_hits) * estimated_rows_filtered
       * index_speedup_factor`.
     - Computes `cost = (insert_count + update_count + delete_count)
       * index_write_overhead`.
     - If `benefit / cost > auto_index.threshold` and no index exists on this column,
       issues `CREATE INDEX CONCURRENTLY` via SPI and records the new index in
       `auto_index_catalog`.
   - **DROP path** — for each index previously created by this extension (rows in
     `auto_index_catalog`):
     - Queries `pg_stat_user_indexes.idx_scan` for that index OID via SPI.
     - If the usage ratio falls below `1 / auto_index.threshold` relative to write
       cost, issues `DROP INDEX CONCURRENTLY` via SPI and removes the catalog row.
4. Resets counters in shared memory (under exclusive LWLock) after each evaluation pass.

**Note:** `CREATE INDEX CONCURRENTLY` cannot run inside a transaction. Use
`SPI_connect_ext(SPI_OPT_NONATOMIC)` to open a non-atomic SPI context in the
background worker before issuing the DDL.

**Note:** The background worker must call
`BackgroundWorkerInitializeConnection(dbname, NULL, 0)` to connect to a specific
database. If you want to manage indexes across multiple databases, you either need
one dynamic bgworker per database or iterate over `pg_database` and reconnect for each.

---

## `contrib/auto_index/auto_index--1.0.sql`

SQL objects installed when `CREATE EXTENSION auto_index` is run:

- `auto_index_catalog` table — records which indexes were created by the extension
  (relid, indexrelid, created_at) so the bgworker can drop them when no longer
  beneficial.
- `auto_index_stats()` set-returning function — calls a C function that reads the
  shared memory segment and returns one row per tracked (table, column) pair with
  all counters. Useful for debugging and monitoring.
- `auto_index_reset()` function — acquires the exclusive LWLock and zeroes all
  counters.

---

## `contrib/auto_index/auto_index.control`

Standard extension control file:
```
default_version = '1.0'
module_pathname = '$libdir/auto_index'
relocatable = false
requires = ''
```

---

## `contrib/auto_index/Makefile`

PGXS-based Makefile:
```makefile
MODULE_big = auto_index
OBJS = auto_index.o
EXTENSION = auto_index
DATA = auto_index--1.0.sql
PGFILEDESC = "auto_index - automatic index management"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/auto_index
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_builddir)/contrib/contrib-global.mk
endif
```

Supports both in-tree builds (`make -C contrib/auto_index`) and out-of-tree builds
with `USE_PGXS=1`.
