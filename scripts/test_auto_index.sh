#!/bin/bash
#
# auto_index performance demonstration.
# Shows query execution time before and after the extension creates indexes.
# Run inside the Docker container:
#   bash scripts/test_auto_index.sh
#
set -e

export PATH=/usr/local/pgsql/bin:$PATH
DB=postgres
PSQL="psql -d $DB -X"

SEP="================================================================"
banner() { echo ""; echo "$SEP"; echo "  $1"; echo "$SEP"; }

# ---------------------------------------------------------------------------
# exec_time <query>
# Returns the median execution time in ms from 5 EXPLAIN ANALYZE runs.
# ---------------------------------------------------------------------------
exec_time() {
    local query="$1"
    local times=()
    for i in 1 2 3 4 5; do
        t=$($PSQL -Atq -c "EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT) $query" \
            | grep "Execution Time" | awk '{print $3}')
        times+=("$t")
    done
    # sort and pick median (3rd of 5)
    echo "${times[@]}" | tr ' ' '\n' | sort -n | awk 'NR==3'
}

# scan_type <query>  — returns "Seq Scan", "Index Scan", etc.
scan_type() {
    $PSQL -Atq -c "EXPLAIN (FORMAT TEXT) $1" | grep -oP '(Seq Scan|Index Scan|Index Only Scan|Bitmap Heap Scan)' | head -1
}

# ---------------------------------------------------------------------------
# Step 1: configure and restart
# ---------------------------------------------------------------------------
banner "Step 1: configuring auto_index and restarting"

$PSQL -c "ALTER SYSTEM SET auto_index.threshold      = 2;"
$PSQL -c "ALTER SYSTEM SET auto_index.check_interval = 10;"

pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s
echo "Server restarted  (threshold=2, check_interval=10s)"

# ---------------------------------------------------------------------------
# Step 2: prepare
# ---------------------------------------------------------------------------
banner "Step 2: preparing test table (200 000 rows)"

$PSQL <<SQL
CREATE EXTENSION IF NOT EXISTS auto_index;

DROP TABLE IF EXISTS perf_test CASCADE;
DELETE FROM auto_index_catalog;
SELECT auto_index_reset();

CREATE TABLE perf_test (
    id          serial PRIMARY KEY,
    amount      int,        -- 0-10 000
    category    int,        -- 0-99  (1% selectivity)
    region      text        -- one of 4 values (25% selectivity)
);

INSERT INTO perf_test (amount, category, region)
SELECT
    (random() * 10000)::int,
    (random() * 99)::int,
    (ARRAY['north','south','east','west'])[(random()*4)::int % 4 + 1]
FROM generate_series(1, 200000);

ANALYZE perf_test;
SQL

echo "Table ready."

# ---------------------------------------------------------------------------
# Step 3: baseline timing BEFORE any indexes
# ---------------------------------------------------------------------------
banner "Step 3: baseline performance WITHOUT indexes"

Q_EQ="SELECT count(*) FROM perf_test WHERE category = 42"
Q_RNG="SELECT count(*) FROM perf_test WHERE amount > 9800"

echo ""
echo "Query A: $Q_EQ"
echo "  Scan type : $(scan_type "$Q_EQ")"
BEFORE_EQ=$(exec_time "$Q_EQ")
echo "  Exec time : ${BEFORE_EQ} ms  (median of 5 runs)"

echo ""
echo "Query B: $Q_RNG"
echo "  Scan type : $(scan_type "$Q_RNG")"
BEFORE_RNG=$(exec_time "$Q_RNG")
echo "  Exec time : ${BEFORE_RNG} ms  (median of 5 runs)"

echo ""
echo "Full plan for Query A:"
$PSQL -c "EXPLAIN (ANALYZE, BUFFERS) $Q_EQ"

# ---------------------------------------------------------------------------
# Step 4: run read workload so the bgworker sees enough hits
# ---------------------------------------------------------------------------
banner "Step 4: running read workload to trigger auto_index"

$PSQL -c "SELECT auto_index_reset();" > /dev/null

for i in $(seq 1 40); do
    $PSQL -q -c "$Q_EQ" > /dev/null
    $PSQL -q -c "$Q_RNG" > /dev/null
done

echo "Ran 40 iterations of each query."
echo ""
echo "Hit counters now in shared memory:"
$PSQL <<SQL
SELECT
    c.relname       AS table_name,
    a.attname       AS column_name,
    s.equality_hits,
    s.range_hits,
    s.equality_hits * 2 + s.range_hits AS benefit
FROM auto_index_stats() s
JOIN pg_class     c ON c.oid      = s.relid
JOIN pg_attribute a ON a.attrelid = s.relid AND a.attnum = s.attno
ORDER BY benefit DESC;
SQL

# ---------------------------------------------------------------------------
# Step 5: wait for bgworker
# ---------------------------------------------------------------------------
banner "Step 5: waiting for background worker to create indexes (~15s)"

for i in $(seq 1 15); do printf "."; sleep 1; done
echo ""

# ---------------------------------------------------------------------------
# Step 6: verify indexes were created
# ---------------------------------------------------------------------------
banner "Step 6: indexes created by auto_index"

$PSQL <<SQL
SELECT
    c.relname       AS table_name,
    ic.relname      AS index_name,
    a.attname       AS column_name,
    ai.created_at
FROM auto_index_catalog ai
JOIN pg_class     c  ON c.oid      = ai.relid
JOIN pg_class     ic ON ic.oid     = ai.indexrelid
JOIN pg_attribute a  ON a.attrelid = ai.relid AND a.attnum = ai.attno
ORDER BY ai.created_at;
SQL

# ---------------------------------------------------------------------------
# Step 7: performance AFTER indexes
# ---------------------------------------------------------------------------
banner "Step 7: performance WITH indexes"

echo ""
echo "Query A: $Q_EQ"
echo "  Scan type : $(scan_type "$Q_EQ")"
AFTER_EQ=$(exec_time "$Q_EQ")
echo "  Exec time : ${AFTER_EQ} ms  (median of 5 runs)"

echo ""
echo "Query B: $Q_RNG"
echo "  Scan type : $(scan_type "$Q_RNG")"
AFTER_RNG=$(exec_time "$Q_RNG")
echo "  Exec time : ${AFTER_RNG} ms  (median of 5 runs)"

echo ""
echo "Full plan for Query A:"
$PSQL -c "EXPLAIN (ANALYZE, BUFFERS) $Q_EQ"

# ---------------------------------------------------------------------------
# Step 8: summary
# ---------------------------------------------------------------------------
banner "Step 8: summary"

speedup_eq=$(awk "BEGIN { if ($AFTER_EQ > 0) printf \"%.1f\", $BEFORE_EQ / $AFTER_EQ; else print \"N/A\" }")
speedup_rng=$(awk "BEGIN { if ($AFTER_RNG > 0) printf \"%.1f\", $BEFORE_RNG / $AFTER_RNG; else print \"N/A\" }")

echo ""
printf "%-12s  %12s  %12s  %10s\n" "Query" "Before (ms)" "After (ms)" "Speedup"
printf "%-12s  %12s  %12s  %10s\n" "------------" "------------" "------------" "----------"
printf "%-12s  %12s  %12s  %10s\n" "category=42"  "$BEFORE_EQ"  "$AFTER_EQ"  "${speedup_eq}x"
printf "%-12s  %12s  %12s  %10s\n" "amount>9800"  "$BEFORE_RNG" "$AFTER_RNG" "${speedup_rng}x"

echo ""
echo "auto_index detected high read/write ratios and created indexes"
echo "automatically — no manual CREATE INDEX required."

# ---------------------------------------------------------------------------
# Step 9: restore
# ---------------------------------------------------------------------------
banner "Step 9: restoring defaults"

$PSQL -c "ALTER SYSTEM RESET auto_index.threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.check_interval;"
$PSQL -c "SELECT pg_reload_conf();" > /dev/null
echo "Done."
