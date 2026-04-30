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
3. **Write tracking** — implement `ProcessUtility_hook` to increment insert/update/delete
   counters; test by running INSERTs and querying the stats function
4. **Read tracking** — implement `ExecutorEnd_hook` to walk the plan tree and record
   seq scan counts and column predicate hits; test with `SELECT ... WHERE` on an unindexed table
5. **Background worker** — get it waking on a latch and logging the stats snapshot;
   verify numbers look correct before adding any DDL
6. **Index decisions** — add the benefit/cost calculation and wire up
   `CREATE INDEX CONCURRENTLY` via SPI; test on a table with a skewed read-heavy workload

---

## `contrib/auto_index/auto_index.h`

Defines the shared memory layout used by every backend and the background worker.

Key structs:
- `AutoIndexColumnStats` — per-column counters: equality_hits, range_hits (from predicate walks)
- `AutoIndexTableStats` — per-table counters: seq_scan_count, index_scan_count, insert_count,
  update_count, delete_count, plus an array of `AutoIndexColumnStats` keyed by attribute number
- `AutoIndexSharedState` — top-level segment: an `LWLock` for mutual exclusion, a fixed-size
  array of `AutoIndexTableStats` entries (sized by `auto_index.max_tracked_tables` at startup),
  a generation counter so the bgworker can detect resets, and an LRU timestamp per entry used
  for eviction when all slots are full

---

## `contrib/auto_index/auto_index.c`

Main extension file. Implements all hooks, the background worker, and the index decision engine.

### `_PG_init()`
Registers GUC parameters (`auto_index.threshold`, `auto_index.check_interval`,
`auto_index.max_indexes_per_table`), chains onto `shmem_request_hook`,
`shmem_startup_hook`, `ExecutorEnd_hook`, and `ProcessUtility_hook`,
and registers the background worker with `RegisterBackgroundWorker()`.

### `auto_index_shmem_request()`
Called during postmaster startup (before `fork()`), before the shared memory segment
is allocated — this is the only window to reserve space.
Calls `RequestAddinShmemSpace(compute_shmem_size())` and
`RequestNamedLWLockTranche("auto_index", 1)` to reserve space for one tranche lock.

`compute_shmem_size()` reads `auto_index.max_tracked_tables` and
`auto_index.max_columns_per_table` at this point to size the allocation.
**These GUCs cannot be changed without a full server restart** — they affect shared
memory layout and are read before any config reload can fire, just like `shared_buffers`
or `max_connections`. Document this constraint clearly.

### `auto_index_shmem_startup()`
Called after shared memory is mapped.
Uses `ShmemInitStruct("auto_index", ...)` to attach to (or create) the segment,
then zero-initialises `AutoIndexSharedState` and stores the `LWLock` pointer from the
named tranche. Chains to any previous `shmem_startup_hook`.

### `auto_index_executor_end(QueryDesc *queryDesc)`
Fires after every query completes.
Walks `queryDesc->planstate` recursively looking for `SeqScan` and `IndexScan` nodes.
For each `SeqScan` node found, acquires the LWLock in exclusive mode and increments
`seq_scan_count` for that table's entry in shared memory.
Also inspects `qual` expressions on the scan node to determine which attribute numbers
appear as simple `Var op Const` predicates, and increments the appropriate column's
`equality_hits` or `range_hits` counter.
Chains to `prev_ExecutorEnd`.

**Note:** Extracting attribute numbers from qual expressions is the most complex part
of the implementation. Walk the `Expr` tree looking for `OpExpr` nodes where one
argument is a `Var` (column reference) and the other is a `Const`. Use `nodeSeqscan.c`
and `ExecSeqScan` as a reference for how quals are structured on the plan node.

### `auto_index_process_utility(PlannedStmt *pstmt, ...)`
Fires for all utility statements.
After calling the previous hook / standard ProcessUtility, checks whether the statement
was an `INSERT`, `UPDATE`, or `DELETE` (via `pstmt->commandType` / `nodeTag`).
Resolves the target relation OID and, under the LWLock, increments the relevant write
counter in shared memory.
Chains to `prev_ProcessUtility`.

### `auto_index_main()` — background worker entry point
Loops forever:
1. Calls `WaitLatch` with the configured check interval.
2. Acquires the LWLock in shared mode, copies the stats snapshot, releases the lock.
3. For each tracked table, calls `evaluate_index_candidates()`:
   - Fetches `pg_class.reltuples` via SPI for table cardinality.
   - Reads `pg_statistic` (n_distinct, correlation) for candidate columns to estimate
     selectivity and index scan cost.
   - Computes `benefit = seq_scan_count * rows_filtered_per_scan * index_speedup_factor`.
   - Computes `cost = (insert_count + delete_count + update_count) * index_write_overhead`.
   - If `benefit / cost > auto_index.threshold` and the column has no index, issues
     `CREATE INDEX CONCURRENTLY` via SPI.
   - If the ratio falls below `1 / auto_index.threshold` for an auto-created index (tracked
     in the `auto_index_catalog` table), issues `DROP INDEX CONCURRENTLY` via SPI.
4. Resets counters in shared memory (under exclusive LWLock) after each evaluation pass.

**Note:** `CREATE INDEX CONCURRENTLY` cannot run inside a transaction. SPI normally
wraps every call in one, so use `SPI_connect_ext(SPI_OPT_NONATOMIC)` to open a
non-atomic SPI context in the background worker before issuing the DDL.

**Note:** The background worker is server-wide (one per postmaster) but the
`auto_index_catalog` table is per-database. The worker must call
`BackgroundWorkerInitializeConnection(dbname, NULL, 0)` to connect to a specific
database. If you want to manage indexes across multiple databases, you either need
one dynamic bgworker per database or iterate over `pg_database` and reconnect for each.

---

## `contrib/auto_index/auto_index--1.0.sql`

SQL objects installed when `CREATE EXTENSION auto_index` is run:

- `auto_index_catalog` table — records which indexes were created by the extension
  (relid, indexrelid, created_at) so the bgworker can drop them when no longer beneficial.
- `auto_index_stats()` set-returning function — calls a C function that reads the shared
  memory segment and returns one row per tracked (table, column) pair with all counters.
  Useful for debugging and monitoring.
- `auto_index_reset()` function — acquires the exclusive LWLock and zeroes all counters.

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
