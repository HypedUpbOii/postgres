#!/bin/bash
#
# Verify auto_index drops indexes that go idle.
#
# Three phases:
#   1. READS: run lots of WHERE col = ? queries to trigger index creation.
#   2. WRITES: stop reading, run pure INSERT/UPDATE/DELETE for several
#      bgworker intervals.  The index sits idle (idx_scan never advances)
#      while the ski-rental drop accumulator grows each interval.
#   3. VERIFY: index is gone, catalog row deleted.
#
# Run inside the container:
#   bash scripts/test_drop.sh
#
# Env vars:
#   ROWS=50000              rows in the test table (smaller table → smaller
#                           build_cost → faster drop)
#   READ_ITERS=100          read queries during phase 1
#   WRITE_ITERS=200         write statements during phase 2
#   CHECK_INTERVAL=5        bgworker wake interval
#   THRESHOLD=2             auto_index.threshold
#   SIMPLE_THRESHOLD=5      use simple_threshold strategy with low bar so
#                           the index is created reliably
#   IDLE_INTERVALS=8        wait this many check_interval ticks before
#                           verifying drop (default = 40s with 5s interval)
#
set -e

export PATH=/usr/local/pgsql/bin:$PATH
DB=postgres
PSQL="psql -d $DB -X"

ROWS=${ROWS:-50000}
READ_ITERS=${READ_ITERS:-100}
WRITE_ITERS=${WRITE_ITERS:-200}
CHECK_INTERVAL=${CHECK_INTERVAL:-5}
THRESHOLD=${THRESHOLD:-2}
SIMPLE_THRESHOLD=${SIMPLE_THRESHOLD:-5}
IDLE_INTERVALS=${IDLE_INTERVALS:-8}

WAIT_FOR_CREATE=$((CHECK_INTERVAL * 2 + 5))
WAIT_FOR_DROP=$((CHECK_INTERVAL * IDLE_INTERVALS))

SEP="================================================================"
banner() { echo ""; echo "$SEP"; echo "  $1"; echo "$SEP"; }

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# List auto-created indexes on drop_test table.  Echoes "none" if empty.
list_drop_indexes() {
    local rows
    rows=$($PSQL -Atq <<SQL
SELECT ic.relname || ' on (' ||
       auto_index_attnames(ai.relid, ai.attnos) || ')'
FROM auto_index_catalog ai
JOIN pg_class     c  ON c.oid       = ai.relid
JOIN pg_class     ic ON ic.oid      = ai.indexrelid
WHERE c.relname = 'drop_test'
ORDER BY ai.created_at;
SQL
)
    if [ -z "$rows" ]; then
        echo "  (none)"
    else
        echo "$rows" | sed 's/^/  /'
    fi
}

show_idle_accumulator() {
    $PSQL <<SQL
SELECT
    ic.relname                                     AS index_name,
    ai.last_checked_idx_scan                       AS last_idx_scan,
    s.idx_scan                                     AS current_idx_scan,
    ai.idle_write_cost                             AS idle_accum,
    age(now(), ai.last_checked_at)                 AS since_check
FROM auto_index_catalog ai
JOIN pg_class                ic ON ic.oid = ai.indexrelid
JOIN pg_class                c  ON c.oid = ai.relid
LEFT JOIN pg_stat_user_indexes s ON s.indexrelid = ai.indexrelid
WHERE c.relname = 'drop_test';
SQL
}

# ---------------------------------------------------------------------------
# Phase 0: setup
# ---------------------------------------------------------------------------
banner "Setup: configure auto_index, build $ROWS-row table"

# Use simple_threshold so creation is predictable (the cost-based ski_rental
# can be flaky on small tables — borderline create_threshold).
$PSQL -c "ALTER SYSTEM SET auto_index.create_strategy   = 'simple_threshold';"
$PSQL -c "ALTER SYSTEM SET auto_index.threshold         = $THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.simple_threshold  = $SIMPLE_THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.check_interval    = $CHECK_INTERVAL;"
pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

$PSQL <<SQL
CREATE EXTENSION IF NOT EXISTS auto_index;
DO \$\$
DECLARE r record;
BEGIN
    FOR r IN SELECT schemaname, indexname FROM pg_indexes
             WHERE indexname LIKE 'auto\\_idx\\_%'
    LOOP
        EXECUTE format('DROP INDEX IF EXISTS %I.%I', r.schemaname, r.indexname);
    END LOOP;
END\$\$;
DELETE FROM auto_index_catalog;
SELECT auto_index_reset();

DROP TABLE IF EXISTS drop_test CASCADE;
CREATE TABLE drop_test (
    id        serial PRIMARY KEY,
    category  int,
    payload   text
);

INSERT INTO drop_test (category, payload)
SELECT (random() * 99)::int,
       md5(random()::text)
FROM generate_series(1, $ROWS);

ANALYZE drop_test;
SQL

echo "drop_test ready: $($PSQL -Atq -c 'SELECT count(*) FROM drop_test') rows."

# ---------------------------------------------------------------------------
# Phase 1: heavy reads → expect index creation
# ---------------------------------------------------------------------------
banner "Phase 1: heavy reads (${READ_ITERS} queries on category)"

for i in $(seq 1 $READ_ITERS); do
    cat=$((RANDOM % 100))
    $PSQL -q -c "SELECT count(*) FROM drop_test WHERE category = $cat" > /dev/null
done

echo "Waiting ${WAIT_FOR_CREATE}s for bgworker to fire..."
sleep $WAIT_FOR_CREATE

echo ""
echo "Indexes auto_index has on drop_test:"
list_drop_indexes

INITIAL_IDX_COUNT=$($PSQL -Atq <<SQL
SELECT count(*) FROM auto_index_catalog ai
JOIN pg_class c ON c.oid = ai.relid
WHERE c.relname = 'drop_test';
SQL
)

if [ "$INITIAL_IDX_COUNT" = "0" ]; then
    echo ""
    echo "WARN: no index created.  Drop test cannot proceed."
    echo "      Try ROWS=200000 or lower SIMPLE_THRESHOLD."
    exit 1
fi

echo ""
echo "Initial accumulator state:"
show_idle_accumulator

# ---------------------------------------------------------------------------
# Phase 2: pure writes — index should go idle and the drop accumulator grow.
# ---------------------------------------------------------------------------
banner "Phase 2: pure writes (${WRITE_ITERS} statements, no SELECTs on category)"

# Only writes — no SELECT WHERE category = touches the index.
# Use UPDATE BY id (PK) so we don't hit the auto-created index on category.
for i in $(seq 1 $WRITE_ITERS); do
    id=$((RANDOM % ROWS + 1))
    new_cat=$((RANDOM % 100))
    $PSQL -q <<SQL > /dev/null
UPDATE drop_test SET payload = md5(random()::text) WHERE id = $id;
INSERT INTO drop_test (category, payload) VALUES ($new_cat, md5(random()::text));
DELETE FROM drop_test WHERE id = $((RANDOM % ROWS + 1));
SQL
done
echo "Wrote ~$((WRITE_ITERS * 3)) statements (UPDATE/INSERT/DELETE).  No reads on category."

# ---------------------------------------------------------------------------
# Wait — bgworker accumulates idle cost each interval and drops when
# idle_write_cost >= drop_threshold.
# ---------------------------------------------------------------------------
banner "Waiting ${WAIT_FOR_DROP}s for drop accumulator to fire"

elapsed=0
while [ $elapsed -lt $WAIT_FOR_DROP ]; do
    sleep $CHECK_INTERVAL
    elapsed=$((elapsed + CHECK_INTERVAL))

    cnt=$($PSQL -Atq <<SQL
SELECT count(*) FROM auto_index_catalog ai
JOIN pg_class c ON c.oid = ai.relid
WHERE c.relname = 'drop_test';
SQL
)
    echo "  t=${elapsed}s  catalog_rows_for_drop_test=${cnt}"
    show_idle_accumulator | sed 's/^/    /'
    if [ "$cnt" = "0" ]; then
        echo "  ✓ drop fired."
        break
    fi
done

# ---------------------------------------------------------------------------
# Phase 3: verify
# ---------------------------------------------------------------------------
banner "Phase 3: result"

FINAL_IDX_COUNT=$($PSQL -Atq <<SQL
SELECT count(*) FROM auto_index_catalog ai
JOIN pg_class c ON c.oid = ai.relid
WHERE c.relname = 'drop_test';
SQL
)

PG_INDEXES=$($PSQL -Atq -c "SELECT count(*) FROM pg_indexes WHERE tablename = 'drop_test' AND indexname LIKE 'auto\\_idx\\_%'")

echo ""
echo "auto_index catalog rows for drop_test : $FINAL_IDX_COUNT (was $INITIAL_IDX_COUNT)"
echo "actual auto-indexes in pg_indexes     : $PG_INDEXES"

if [ "$FINAL_IDX_COUNT" = "0" ] && [ "$PG_INDEXES" = "0" ]; then
    echo ""
    echo "✓ PASS: index was created during reads, then dropped during writes."
else
    echo ""
    echo "✗ FAIL: index still present.  The drop accumulator may not have"
    echo "        reached drop_threshold (= build_cost × 1000).  Try"
    echo "        IDLE_INTERVALS=20 or larger."
fi

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
banner "Cleanup"
$PSQL -c "DROP TABLE drop_test CASCADE;" > /dev/null
$PSQL -c "DELETE FROM auto_index_catalog;" > /dev/null
$PSQL -c "ALTER SYSTEM RESET auto_index.create_strategy;"
$PSQL -c "ALTER SYSTEM RESET auto_index.threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.simple_threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.check_interval;"
$PSQL -c "SELECT pg_reload_conf();" > /dev/null
echo "Done."
