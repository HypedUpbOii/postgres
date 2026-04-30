#!/bin/bash
#
# auto_index pgbench demo.
#
# Runs a custom pgbench workload (random equality lookups on an unindexed
# column) and prints TPS every 2 seconds.  You should see the TPS step up
# when the auto_index bgworker creates the index mid-run.
#
# Run inside the Docker container:
#   bash scripts/test_pgbench.sh
#
# Tunables (env-overridable):
#   ROWS=1000000        rows in the test table
#   DURATION=60         pgbench run length in seconds
#   CLIENTS=4           pgbench concurrent clients (-c)
#   JOBS=2              pgbench worker threads (-j)
#   PROGRESS=2          TPS progress interval in seconds (-P)
#   CHECK_INTERVAL=5    auto_index bgworker wake interval (seconds)
#   THRESHOLD=2         auto_index.threshold (read/write ratio gate)
#
set -e

export PATH=/usr/local/pgsql/bin:$PATH
DB=postgres
PSQL="psql -d $DB -X"

ROWS=${ROWS:-1000000}
DURATION=${DURATION:-60}
CLIENTS=${CLIENTS:-4}
JOBS=${JOBS:-2}
PROGRESS=${PROGRESS:-2}
CHECK_INTERVAL=${CHECK_INTERVAL:-5}
THRESHOLD=${THRESHOLD:-2}

SEP="================================================================"
banner() { echo ""; echo "$SEP"; echo "  $1"; echo "$SEP"; }

# ---------------------------------------------------------------------------
# Step 1: configure auto_index and restart so GUCs take effect
# ---------------------------------------------------------------------------
banner "Step 1: configuring auto_index (threshold=$THRESHOLD, check_interval=${CHECK_INTERVAL}s)"

$PSQL -c "ALTER SYSTEM SET auto_index.threshold      = $THRESHOLD;"
$PSQL -c "ALTER SYSTEM SET auto_index.check_interval = $CHECK_INTERVAL;"

pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s
echo "Server restarted."

# ---------------------------------------------------------------------------
# Step 2: prepare test table
# ---------------------------------------------------------------------------
banner "Step 2: preparing test table ($ROWS rows)"

$PSQL <<SQL
CREATE EXTENSION IF NOT EXISTS auto_index;

DROP TABLE IF EXISTS bench_test CASCADE;
DELETE FROM auto_index_catalog;
SELECT auto_index_reset();

CREATE TABLE bench_test (
    id          serial PRIMARY KEY,
    amount      int,
    category    int,
    region      text
);

INSERT INTO bench_test (amount, category, region)
SELECT
    (random() * 10000)::int,
    (random() * 99)::int,
    (ARRAY['north','south','east','west'])[(random()*4)::int % 4 + 1]
FROM generate_series(1, $ROWS);

ANALYZE bench_test;
SQL

echo "Table ready ($ROWS rows, no index on category/amount)."

# ---------------------------------------------------------------------------
# Step 3: pgbench workload script
# ---------------------------------------------------------------------------
SCRIPT=/tmp/auto_index_bench.sql
cat > $SCRIPT <<'PGB'
\set cat random(0, 99)
SELECT count(*) FROM bench_test WHERE category = :cat;
PGB

banner "Step 3: pgbench workload"
echo "Script ($SCRIPT):"
sed 's/^/    /' $SCRIPT
echo ""
echo "  ${CLIENTS} clients × ${JOBS} jobs, ${DURATION}s, progress every ${PROGRESS}s"
echo ""
echo "  Watch the 'tps' column:"
echo "    - First ~10s should show low TPS (sequential scans of $ROWS rows)"
echo "    - When auto_index creates the index, TPS jumps sharply"
echo ""

# ---------------------------------------------------------------------------
# Step 4: run pgbench
# ---------------------------------------------------------------------------
pgbench -d $DB \
        -n \
        -c $CLIENTS \
        -j $JOBS \
        -T $DURATION \
        -P $PROGRESS \
        -f $SCRIPT

# ---------------------------------------------------------------------------
# Step 5: show what auto_index decided
# ---------------------------------------------------------------------------
banner "Step 5: indexes created by auto_index"

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

echo ""
echo "Final hit counters in shared memory (read this BEFORE auto_index_reset!):"
$PSQL <<SQL
SELECT
    c.relname       AS table_name,
    a.attname       AS column_name,
    s.equality_hits,
    s.range_hits
FROM auto_index_stats() s
JOIN pg_class     c ON c.oid      = s.relid
JOIN pg_attribute a ON a.attrelid = s.relid AND a.attnum = s.attno
ORDER BY s.equality_hits + s.range_hits DESC;
SQL

# ---------------------------------------------------------------------------
# Step 6: restore defaults
# ---------------------------------------------------------------------------
banner "Step 6: restoring GUCs"

$PSQL -c "ALTER SYSTEM RESET auto_index.threshold;"
$PSQL -c "ALTER SYSTEM RESET auto_index.check_interval;"
$PSQL -c "SELECT pg_reload_conf();" > /dev/null
echo "Done."
