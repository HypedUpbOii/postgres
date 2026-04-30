#!/bin/bash
#
# Run the pgbench-based auto_index demo inside the running container.
# Forwards env-var tunables (ROWS, DURATION, CLIENTS, JOBS, PROGRESS,
# CHECK_INTERVAL, THRESHOLD) — see scripts/test_pgbench.sh for defaults.
#
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Running auto_index pgbench demo..."

docker compose exec \
    -e ROWS \
    -e DURATION \
    -e CLIENTS \
    -e JOBS \
    -e PROGRESS \
    -e CHECK_INTERVAL \
    -e THRESHOLD \
    $CONTAINER_NAME bash -c "cd /postgres && bash scripts/test_pgbench.sh"
