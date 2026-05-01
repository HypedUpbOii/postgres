#!/bin/bash
#
# TPC-C-lite OLTP benchmark with auto_index.
#
# Three phases:
#   Phase A: BASELINE  — auto_index threshold huge, never fires.  pgbench
#                        for $DURATION_S seconds with TPC-C-style txn mix.
#   Phase B: TRAINING  — set strategy/threshold, run shorter pgbench so
#                        bgworker can see the workload.
#   Phase C: INDEXED   — full $DURATION_S pgbench with auto_index's indexes.
#
# OLTP ≠ OLAP: speedups will be smaller than TPC-H.  The win is on
# OrderStatus (lookup-by-last-name, ~60% of read xns) and any seq-scanned
# joins.  Writes get *slower* per-statement because of index maintenance,
# so the net throughput depends on the read/write mix.
#
# Run inside the container:
#   bash scripts/test_tpcc.sh
#
# Env vars:
#   SCALE=2                 number of warehouses (TPC-C uses :scale)
#   DURATION_S=30           pgbench duration per phase (seconds)
#   TRAINING_S=15           training pgbench duration
#   CLIENTS=4               pgbench -c
#   JOBS=2                  pgbench -j
#   STRATEGY=size_gated     auto_index.create_strategy
#   SIMPLE_THRESHOLD=20     auto_index.simple_threshold
#   THRESHOLD=2             auto_index.threshold
#   CHECK_INTERVAL=5
#   WAIT_FOR_BGWORKER=15
#
set -e

export PATH=/usr/local/pgsql/bin:$PATH
DB=postgres
PSQL="psql -d $DB -X"

SCALE=${SCALE:-2}
DURATION_S=${DURATION_S:-30}
TRAINING_S=${TRAINING_S:-15}
CLIENTS=${CLIENTS:-4}
JOBS=${JOBS:-2}
STRATEGY=${STRATEGY:-size_gated}
SIMPLE_THRESHOLD=${SIMPLE_THRESHOLD:-20}
THRESHOLD=${THRESHOLD:-2}
CHECK_INTERVAL=${CHECK_INTERVAL:-5}
WAIT_FOR_BGWORKER=${WAIT_FOR_BGWORKER:-15}
BASELINE_THRESHOLD=1000

XN_DIR=/postgres/scripts/tpcc_xn
RESULT_DIR=/tmp/tpcc_results
mkdir -p "$RESULT_DIR"

SEP="================================================================"
banner() { echo ""; echo "$SEP"; echo "  $1"; echo "$SEP"; }

# Transaction weights.  Read-heavy on order_status here (vs canonical 4%)
# so auto_index has a chance to observe seq-scans.  Writes still dominate.
PGB_ARGS=(
    -d "$DB" -n
    -c "$CLIENTS" -j "$JOBS"
    --define=scale="$SCALE"
    -f "$XN_DIR/new_order.sql@40"
    -f "$XN_DIR/payment.sql@30"
    -f "$XN_DIR/order_status.sql@20"
    -f "$XN_DIR/stock_level.sql@10"
)

# ---------------------------------------------------------------------------
# Setup: schema + data
# ---------------------------------------------------------------------------
banner "Setup: TPC-C-lite schema, $SCALE warehouses"

$PSQL -c "ALTER SYSTEM SET auto_index.threshold        = $BASELINE_THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.create_strategy  = 'ski_rental';"
$PSQL -c "ALTER SYSTEM SET auto_index.simple_threshold = $SIMPLE_THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.check_interval   = $CHECK_INTERVAL;"
pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

$PSQL <<SQL > /dev/null
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
SQL

$PSQL -f /postgres/scripts/tpcc_schema.sql > /dev/null
$PSQL -v wh=$SCALE -f /postgres/scripts/tpcc_load.sql > /dev/null

cust=$($PSQL -Atq -c "SELECT count(*) FROM customer")
ords=$($PSQL -Atq -c "SELECT count(*) FROM orders")
ols=$($PSQL -Atq -c "SELECT count(*) FROM order_line")
echo "Loaded: $SCALE warehouses, $cust customers, $ords orders, $ols order_lines."

# ---------------------------------------------------------------------------
# Phase A: BASELINE
# ---------------------------------------------------------------------------
banner "Phase A: BASELINE  (no auto_index)  — ${DURATION_S}s"

# Reset state for fairness, then run.
$PSQL -c "SELECT auto_index_reset();" > /dev/null
$PSQL -c "DELETE FROM auto_index_catalog;" > /dev/null

pgbench "${PGB_ARGS[@]}" -T "$DURATION_S" -P 5 2>&1 | tee "$RESULT_DIR/baseline.txt" | grep -E '^(progress:|tps =|latency average)'

baseline_tps=$(grep '^tps =' "$RESULT_DIR/baseline.txt" | awk '{print $3}')
baseline_lat=$(grep '^latency average' "$RESULT_DIR/baseline.txt" | awk '{print $4}')

# ---------------------------------------------------------------------------
# Phase B: TRAINING — turn auto_index on
# ---------------------------------------------------------------------------
banner "Phase B: TRAINING  (strategy=$STRATEGY, threshold=$THRESHOLD)  — ${TRAINING_S}s"

$PSQL -c "ALTER SYSTEM SET auto_index.threshold        = $THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.create_strategy  = '$STRATEGY';"
$PSQL -c "ALTER SYSTEM SET auto_index.simple_threshold = $SIMPLE_THRESHOLD;"
pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

$PSQL -c "SELECT auto_index_reset();" > /dev/null
$PSQL -c "DELETE FROM auto_index_catalog;" > /dev/null

pgbench "${PGB_ARGS[@]}" -T "$TRAINING_S" -P 5 > /dev/null 2>&1 || true

echo "Waiting ${WAIT_FOR_BGWORKER}s for bgworker..."
sleep $WAIT_FOR_BGWORKER

echo ""
echo "Indexes auto_index created during training:"
$PSQL <<SQL
SELECT
    c.relname                                  AS table_name,
    ic.relname                                 AS index_name,
    auto_index_attnames(ai.relid, ai.attnos)   AS columns,
    array_length(ai.attnos, 1)                 AS n_cols,
    ai.created_at
FROM auto_index_catalog ai
JOIN pg_class     c  ON c.oid  = ai.relid
JOIN pg_class     ic ON ic.oid = ai.indexrelid
ORDER BY ai.created_at;
SQL

# ---------------------------------------------------------------------------
# Phase C: INDEXED
# ---------------------------------------------------------------------------
banner "Phase C: INDEXED  — ${DURATION_S}s"

pgbench "${PGB_ARGS[@]}" -T "$DURATION_S" -P 5 2>&1 | tee "$RESULT_DIR/indexed.txt" | grep -E '^(progress:|tps =|latency average)'

indexed_tps=$(grep '^tps =' "$RESULT_DIR/indexed.txt" | awk '{print $3}')
indexed_lat=$(grep '^latency average' "$RESULT_DIR/indexed.txt" | awk '{print $4}')

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
banner "Summary"

speedup=$(awk -v b="$baseline_tps" -v i="$indexed_tps" 'BEGIN { if (b > 0) printf "%.2fx", i/b; else print "N/A" }')

printf "%-12s  %14s  %14s\n"   "Phase"     "TPS"             "Latency (ms)"
printf "%-12s  %14s  %14s\n"   "------"    "-------------"   "-------------"
printf "%-12s  %14s  %14s\n"   "Baseline"  "$baseline_tps"   "$baseline_lat"
printf "%-12s  %14s  %14s  (%sx vs baseline)\n" "Indexed"  "$indexed_tps" "$indexed_lat" "$speedup"

echo ""
echo "Note: in OLTP a 1.0x or modest speedup is the EXPECTED outcome.  Index"
echo "      maintenance taxes every write (here ~88% of the workload).  The"
echo "      win comes from the read txns (OrderStatus by last name)."

# ---------------------------------------------------------------------------
# Cleanup GUCs (leave data + indexes — easy to inspect)
# ---------------------------------------------------------------------------
banner "Restoring GUCs (data + indexes left in place for inspection)"

$PSQL -c "ALTER SYSTEM RESET auto_index.threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.check_interval;"
$PSQL -c "ALTER SYSTEM RESET auto_index.create_strategy;"
$PSQL -c "ALTER SYSTEM RESET auto_index.simple_threshold;"
$PSQL -c "SELECT pg_reload_conf();" > /dev/null
echo "Done."
