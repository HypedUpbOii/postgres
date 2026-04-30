#!/bin/bash
#
# Compare every auto_index.create_strategy on the same TPC-H workload.
#
# Flow:
#   1. Run baseline once (auto_index.threshold huge, no strategy fires).
#   2. For each strategy: drop existing auto-indexes, reset state, set
#      strategy GUC, restart, run training passes, wait, measure.
#   3. Print comparison table.
#
# Knobs that affect every strategy fairly are set low so all strategies
# can plausibly fire.  Knobs unique to a strategy are also set so they
# don't accidentally lock out that strategy.
#
# Run inside the container:
#   bash scripts/test_strategies.sh
#
# Env vars:
#   STRATEGIES="ski_rental simple_threshold ratio_only cost_gain size_gated always"
#   ITERATIONS=1                  per-phase repetitions per query (median)
#   TRAINING_PASSES=3             query passes during training
#   CHECK_INTERVAL=5              bgworker wake interval
#   THRESHOLD=2                   auto_index.threshold (ski_rental, ratio_only)
#   SIMPLE_THRESHOLD=5            auto_index.simple_threshold (simple/size_gated)
#   MIN_TABLE_ROWS=1000           auto_index.min_table_rows (size_gated)
#   WAIT_FOR_BGWORKER=20          seconds after training before measuring
#
set -e

export PATH=/usr/local/pgsql/bin:$PATH
DB=postgres
PSQL="psql -d $DB -X"

STRATEGIES=${STRATEGIES:-"ski_rental simple_threshold ratio_only cost_gain size_gated always"}
ITERATIONS=${ITERATIONS:-1}
TRAINING_PASSES=${TRAINING_PASSES:-3}
CHECK_INTERVAL=${CHECK_INTERVAL:-5}
THRESHOLD=${THRESHOLD:-2}
SIMPLE_THRESHOLD=${SIMPLE_THRESHOLD:-5}
MIN_TABLE_ROWS=${MIN_TABLE_ROWS:-1000}
WAIT_FOR_BGWORKER=${WAIT_FOR_BGWORKER:-20}
BASELINE_THRESHOLD=1000

QUERIES_DIR=/postgres/scripts/tpch_queries
RESULT_DIR=/tmp/tpch_strategy_results
mkdir -p "$RESULT_DIR"

SEP="================================================================"
banner() { echo ""; echo "$SEP"; echo "  $1"; echo "$SEP"; }

# Sanity
ROWS=$($PSQL -Atq -c "SELECT count(*) FROM lineitem" 2>/dev/null || echo 0)
if [ "$ROWS" = "0" ]; then
    echo "ERROR: lineitem empty.  Run scripts/setup_tpch.sh first." >&2
    exit 1
fi
echo "lineitem has $ROWS rows."

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Median of ITERATIONS EXPLAIN ANALYZE runs in milliseconds.
# Echoes the median ms.
exec_time() {
    local qfile="$1"
    local times=()
    for i in $(seq 1 $ITERATIONS); do
        local out
        out=$($PSQL -Atq -c "EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT) $(cat $qfile)" 2>/dev/null || echo "")
        local t
        t=$(echo "$out" | grep "Execution Time" | awk '{print $3}')
        if [ -z "$t" ]; then
            times+=("-1")
        else
            times+=("$t")
        fi
    done
    echo "${times[@]}" | tr ' ' '\n' | sort -n | awk -v n=$ITERATIONS 'NR==(int((n+1)/2))'
}

# Run all queries; write "qname|ms" lines to outfile.  Optionally silent.
run_phase() {
    local outfile="$1"
    local silent="${2:-0}"
    : > "$outfile"
    for qfile in $(ls $QUERIES_DIR/q*.sql | sort); do
        local qname
        qname=$(basename "$qfile" .sql)
        local ms
        ms=$(exec_time "$qfile")
        echo "${qname}|${ms}" >> "$outfile"
        if [ "$silent" != "1" ]; then
            printf "  %-6s  %10s ms\n" "$qname" "$ms"
        fi
    done
}

# Wipe any existing auto-created indexes + catalog rows + shmem state.
reset_auto_index_state() {
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
}

# ---------------------------------------------------------------------------
# Phase 0: BASELINE — same for every strategy
# ---------------------------------------------------------------------------
banner "Phase 0: BASELINE (auto_index disabled by huge threshold)"

$PSQL -c "ALTER SYSTEM SET auto_index.threshold        = $BASELINE_THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.check_interval   = $CHECK_INTERVAL;"
$PSQL -c "ALTER SYSTEM SET auto_index.create_strategy  = 'ski_rental';"
$PSQL -c "ALTER SYSTEM SET auto_index.simple_threshold = $SIMPLE_THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.min_table_rows   = $MIN_TABLE_ROWS;"

pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

reset_auto_index_state
run_phase "$RESULT_DIR/baseline.txt"

# ---------------------------------------------------------------------------
# Phase 1..N: each strategy
# ---------------------------------------------------------------------------
declare -A IDX_COUNTS

for strategy in $STRATEGIES; do
    banner "Strategy: $strategy"

    $PSQL -c "ALTER SYSTEM SET auto_index.create_strategy  = '$strategy';"
    $PSQL -c "ALTER SYSTEM SET auto_index.threshold        = $THRESHOLD;"
    $PSQL -c "ALTER SYSTEM SET auto_index.check_interval   = $CHECK_INTERVAL;"
    $PSQL -c "ALTER SYSTEM SET auto_index.simple_threshold = $SIMPLE_THRESHOLD;"
    $PSQL -c "ALTER SYSTEM SET auto_index.min_table_rows   = $MIN_TABLE_ROWS;"

    pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

    reset_auto_index_state

    echo "Training ($TRAINING_PASSES passes)..."
    for p in $(seq 1 $TRAINING_PASSES); do
        run_phase "$RESULT_DIR/_t.txt" 1 > /dev/null
    done

    echo "Waiting ${WAIT_FOR_BGWORKER}s for bgworker..."
    sleep $WAIT_FOR_BGWORKER

    # Inspect what was created.
    cnt=$($PSQL -Atq -c "SELECT count(*) FROM auto_index_catalog WHERE relid <> 'auto_index_catalog'::regclass")
    cnt_self=$($PSQL -Atq -c "SELECT count(*) FROM auto_index_catalog WHERE relid =  'auto_index_catalog'::regclass")
    IDX_COUNTS[$strategy]="${cnt}+${cnt_self}"
    echo "Indexes created on TPC-H tables: $cnt  (plus $cnt_self on auto_index_catalog itself)"

    $PSQL <<SQL
SELECT c.relname                                  AS table_name,
       ic.relname                                 AS index_name,
       auto_index_attnames(ai.relid, ai.attnos)   AS columns,
       array_length(ai.attnos, 1)                 AS n_cols
FROM auto_index_catalog ai
JOIN pg_class     c  ON c.oid = ai.relid
JOIN pg_class     ic ON ic.oid = ai.indexrelid
WHERE ai.relid <> 'auto_index_catalog'::regclass
ORDER BY ai.created_at;
SQL

    echo "Measuring with strategy=$strategy:"
    run_phase "$RESULT_DIR/strategy_${strategy}.txt"
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
banner "Comparison: per-query speedup vs baseline"

# Header.
printf "%-6s  %12s" "Query" "Baseline ms"
for strategy in $STRATEGIES; do
    printf "  %12s" "$strategy"
done
echo ""

printf "%-6s  %12s" "------" "-----------"
for strategy in $STRATEGIES; do
    printf "  %12s" "------------"
done
echo ""

# Body.  For each query, baseline ms then a speedup column per strategy.
queries=$(awk -F'|' '{print $1}' "$RESULT_DIR/baseline.txt")
for q in $queries; do
    base=$(awk -F'|' -v q="$q" '$1==q {print $2}' "$RESULT_DIR/baseline.txt")
    printf "%-6s  %12s" "$q" "$base"
    for strategy in $STRATEGIES; do
        v=$(awk -F'|' -v q="$q" '$1==q {print $2}' "$RESULT_DIR/strategy_${strategy}.txt")
        if [ -z "$v" ] || [ "$v" = "-1" ] || [ "$base" = "-1" ]; then
            printf "  %12s" "ERR"
        else
            speedup=$(awk -v b="$base" -v v="$v" 'BEGIN {
                if (v <= 0) { print "INF"; exit }
                printf "%.2fx", b / v
            }')
            printf "  %12s" "$speedup"
        fi
    done
    echo ""
done

# Geomean of speedups.
printf "%-6s  %12s" "geo" ""
for strategy in $STRATEGIES; do
    g=$(paste -d'|' "$RESULT_DIR/baseline.txt" "$RESULT_DIR/strategy_${strategy}.txt" | \
        awk -F'|' '
            $2 > 0 && $4 > 0 { s = $2 / $4; sum += log(s); n++ }
            END { if (n > 0) printf "%.2fx", exp(sum / n); else print "N/A" }')
    printf "  %12s" "$g"
done
echo ""

# Index count row.
printf "%-6s  %12s" "idx" ""
for strategy in $STRATEGIES; do
    printf "  %12s" "${IDX_COUNTS[$strategy]}"
done
echo ""

echo ""
echo "Legend:"
echo "  Speedup = baseline_ms / strategy_ms   (>1 = strategy is faster)"
echo "  geo     = geometric mean of per-query speedups"
echo "  idx     = N+M  where N = indexes on TPC-H tables,"
echo "                       M = indexes auto_index made on its own catalog"

# ---------------------------------------------------------------------------
# Restore defaults
# ---------------------------------------------------------------------------
banner "Restoring GUCs"
$PSQL -c "ALTER SYSTEM RESET auto_index.create_strategy;"
$PSQL -c "ALTER SYSTEM RESET auto_index.threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.check_interval;"
$PSQL -c "ALTER SYSTEM RESET auto_index.simple_threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.min_table_rows;"
$PSQL -c "SELECT pg_reload_conf();" > /dev/null
echo "Done."
