#!/bin/bash
#
# Run the strategy comparison inside the running container.
# Forwards env-var tunables — see scripts/test_strategies.sh for defaults.
#
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Comparing auto_index strategies on TPC-H..."

docker compose exec \
    -e STRATEGIES \
    -e ITERATIONS \
    -e TRAINING_PASSES \
    -e CHECK_INTERVAL \
    -e THRESHOLD \
    -e SIMPLE_THRESHOLD \
    -e MIN_TABLE_ROWS \
    -e WAIT_FOR_BGWORKER \
    $CONTAINER_NAME bash -c "cd /postgres && bash scripts/test_strategies.sh"
