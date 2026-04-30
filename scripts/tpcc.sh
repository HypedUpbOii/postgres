#!/bin/bash
#
# Run the TPC-C-lite benchmark inside the running container.
# Forwards env-var tunables — see scripts/test_tpcc.sh for defaults.
#
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Running TPC-C-lite benchmark..."

docker compose exec \
    -e SCALE \
    -e DURATION_S \
    -e TRAINING_S \
    -e CLIENTS \
    -e JOBS \
    -e STRATEGY \
    -e SIMPLE_THRESHOLD \
    -e THRESHOLD \
    -e CHECK_INTERVAL \
    -e WAIT_FOR_BGWORKER \
    $CONTAINER_NAME bash -c "cd /postgres && bash scripts/test_tpcc.sh"
