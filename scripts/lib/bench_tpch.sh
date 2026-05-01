#!/bin/bash
#
# TPC-H benchmark with auto_index.
#
# Three phases:
#   Phase A: BASELINE  — auto_index loaded but threshold huge so it never
#                        fires.  Run all queries, time each.
#   Phase B: TRAINING  — set threshold low, reset state, run all queries
#                        once.  This is what the bgworker watches.
#   Phase C: INDEXED   — wait for the bgworker to act, then run all queries
#                        again.  These are the "with auto_index" timings.
#
# Run inside the container:
#   bash scripts/test_tpch.sh
#
# Env vars:
#   ITERATIONS=1                  per-phase repetitions per query (median)
#   CHECK_INTERVAL=5              auto_index bgworker wake interval, seconds
#   THRESHOLD=2                   auto_index.threshold during phase B/C
#   BASELINE_THRESHOLD=1000       max allowed threshold for phase A (no fires
#                                 in practice: per-column cumulative_benefit
#                                 over a single training pass tops out at
#                                 a few dozen, well below 1000)
#   TRAINING_PASSES=3             how many times to run all queries during
#                                 the training phase.  At SF=1+ a single pass
#                                 is borderline against ski_rental's
#                                 cost-based create_threshold.
#   WAIT_FOR_BGWORKER=20          seconds to sleep after training
#
set -e

export PATH=/usr/local/pgsql/bin:$PATH
DB=postgres
PSQL="psql -d $DB -X"

ITERATIONS=${ITERATIONS:-1}
CHECK_INTERVAL=${CHECK_INTERVAL:-5}
THRESHOLD=${THRESHOLD:-2}
BASELINE_THRESHOLD=${BASELINE_THRESHOLD:-1000}
TRAINING_PASSES=${TRAINING_PASSES:-3}
WAIT_FOR_BGWORKER=${WAIT_FOR_BGWORKER:-20}

QUERIES_DIR=/postgres/scripts/tpch_queries
RESULT_DIR=/tmp/tpch_results
mkdir -p "$RESULT_DIR"

SEP="================================================================"
banner() { echo ""; echo "$SEP"; echo "  $1"; echo "$SEP"; }

# Sanity: queries exist?
if [ ! -d "$QUERIES_DIR" ] || [ -z "$(ls -A $QUERIES_DIR/*.sql 2>/dev/null)" ]; then
    echo "ERROR: no .sql files in $QUERIES_DIR" >&2
    exit 1
fi

# Sanity: data loaded?
ROWS=$($PSQL -Atq -c "SELECT count(*) FROM lineitem" 2>/dev/null || echo 0)
if [ "$ROWS" = "0" ]; then
    echo "ERROR: lineitem is empty.  Run scripts/setup_tpch.sh first." >&2
    exit 1
fi
echo "lineitem has $ROWS rows."

# ---------------------------------------------------------------------------
# Helper: median of ITERATIONS EXPLAIN ANALYZE runs in milliseconds.
# Echoes "<ms>|<scan_summary>".
# ---------------------------------------------------------------------------
exec_time() {
    local qfile="$1"
    local times=()
    local scan_summary=""

    for i in $(seq 1 $ITERATIONS); do
        # EXPLAIN ANALYZE wraps the whole query.
        local out
        out=$($PSQL -Atq -c "EXPLAIN (ANALYZE, BUFFERS, FORMAT TEXT) $(cat $qfile)" 2>/dev/null || echo "")
        local t
        t=$(echo "$out" | grep "Execution Time" | awk '{print $3}')
        if [ -z "$t" ]; then
            # Query failed — record as -1 so it shows up in the table.
            times+=("-1")
        else
            times+=("$t")
        fi
        if [ -z "$scan_summary" ]; then
            # Snapshot the scan-type set on first iteration.
            scan_summary=$(echo "$out" | grep -oE '(Seq Scan|Index Scan|Index Only Scan|Bitmap Heap Scan)' | sort -u | tr '\n' ',' | sed 's/,$//')
        fi
    done

    local median
    median=$(echo "${times[@]}" | tr ' ' '\n' | sort -n | awk -v n=$ITERATIONS 'NR==(int((n+1)/2))')
    echo "${median}|${scan_summary}"
}

# ---------------------------------------------------------------------------
# Helper: run all queries in a phase, write results to a file.
# Args: $1 = phase label, $2 = output file
# ---------------------------------------------------------------------------
run_phase() {
    local label="$1"
    local outfile="$2"

    : > "$outfile"
    for qfile in $(ls $QUERIES_DIR/q*.sql | sort); do
        local qname
        qname=$(basename "$qfile" .sql)
        local result
        result=$(exec_time "$qfile")
        local ms="${result%%|*}"
        local scans="${result##*|}"
        printf "  %-6s  %10s ms   scans: %s\n" "$qname" "$ms" "$scans"
        echo "${qname}|${ms}|${scans}" >> "$outfile"
    done
}

# ---------------------------------------------------------------------------
# Phase A: BASELINE — auto_index disabled (impossibly high threshold)
# ---------------------------------------------------------------------------
banner "Phase A: BASELINE  (auto_index.threshold = $BASELINE_THRESHOLD, will not fire)"

$PSQL -c "ALTER SYSTEM SET auto_index.threshold      = $BASELINE_THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.check_interval = $CHECK_INTERVAL;"
pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

# Make sure no indexes from a prior run linger.
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

run_phase "baseline" "$RESULT_DIR/baseline.txt"

# ---------------------------------------------------------------------------
# Phase B: TRAINING — turn auto_index ON, run queries, let bgworker observe.
# ---------------------------------------------------------------------------
banner "Phase B: TRAINING  (auto_index.threshold = $THRESHOLD, check_interval = ${CHECK_INTERVAL}s)"

$PSQL -c "ALTER SYSTEM SET auto_index.threshold      = $THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.check_interval = $CHECK_INTERVAL;"
pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

$PSQL <<SQL > /dev/null
SELECT auto_index_reset();
DELETE FROM auto_index_catalog;
SQL

# Multiple passes to push cumulative_benefit comfortably above the
# ski_rental cost threshold.  At SF=1 a single pass is right on the edge.
echo "Training: ${TRAINING_PASSES} pass(es) (timings ignored — these populate auto_index counters):"
for p in $(seq 1 $TRAINING_PASSES); do
    run_phase "training-${p}" "$RESULT_DIR/training-${p}.txt" > /dev/null
done

# ---------------------------------------------------------------------------
# Wait for bgworker
# ---------------------------------------------------------------------------
banner "Waiting ${WAIT_FOR_BGWORKER}s for bgworker to create indexes"
for i in $(seq 1 $WAIT_FOR_BGWORKER); do
    printf "."
    sleep 1
done
echo ""

echo ""
echo "Indexes auto_index created during training:"
$PSQL <<SQL
SELECT
    c.relname                                    AS table_name,
    ic.relname                                   AS index_name,
    auto_index_attnames(ai.relid, ai.attnos)     AS columns,
    array_length(ai.attnos, 1)                   AS n_cols,
    ai.created_at
FROM auto_index_catalog ai
JOIN pg_class     c  ON c.oid  = ai.relid
JOIN pg_class     ic ON ic.oid = ai.indexrelid
ORDER BY ai.created_at;
SQL

# ---------------------------------------------------------------------------
# Phase C: INDEXED — measure with auto_index's indexes in place.
# ---------------------------------------------------------------------------
banner "Phase C: INDEXED  (timings with auto_index indexes)"

run_phase "indexed" "$RESULT_DIR/indexed.txt"

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
banner "Summary"

printf "%-6s  %14s  %14s  %10s  %s\n" "Query" "Baseline (ms)" "Indexed (ms)" "Speedup" "Indexed scan"
printf "%-6s  %14s  %14s  %10s  %s\n" "------" "--------------" "--------------" "----------" "------------"

paste -d'|' "$RESULT_DIR/baseline.txt" "$RESULT_DIR/indexed.txt" | \
while IFS='|' read -r q1 b _scans1 q2 i scans2; do
    if [ "$b" = "-1" ] || [ "$i" = "-1" ]; then
        speedup="ERR"
    elif awk "BEGIN { exit !(${i} > 0) }"; then
        speedup=$(awk "BEGIN { printf \"%.1fx\", ${b}/${i} }")
    else
        speedup="N/A"
    fi
    printf "%-6s  %14s  %14s  %10s  %s\n" "$q1" "$b" "$i" "$speedup" "$scans2"
done

echo ""
echo "Detailed timings: $RESULT_DIR/{baseline,indexed}.txt"

# ---------------------------------------------------------------------------
# Restore
# ---------------------------------------------------------------------------
banner "Restoring GUCs"
$PSQL -c "ALTER SYSTEM RESET auto_index.threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.check_interval;"
$PSQL -c "SELECT pg_reload_conf();" > /dev/null
echo "Done."
