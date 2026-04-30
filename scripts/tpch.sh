#!/bin/bash
#
# Run the TPC-H benchmark inside the running container.  Forwards env-var
# tunables — see scripts/test_tpch.sh for defaults.
#
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Running TPC-H benchmark..."

docker compose exec \
    -e ITERATIONS \
    -e CHECK_INTERVAL \
    -e THRESHOLD \
    -e BASELINE_THRESHOLD \
    -e WAIT_FOR_BGWORKER \
    $CONTAINER_NAME bash -c "cd /postgres && bash scripts/test_tpch.sh"
