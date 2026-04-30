#!/bin/bash
#
# Run the write-heavy drop test inside the running container.
# Forwards env-var tunables — see scripts/test_drop.sh for defaults.
#
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Running auto_index drop test..."

docker compose exec \
    -e ROWS \
    -e READ_ITERS \
    -e WRITE_ITERS \
    -e CHECK_INTERVAL \
    -e THRESHOLD \
    -e SIMPLE_THRESHOLD \
    -e IDLE_INTERVALS \
    $CONTAINER_NAME bash -c "cd /postgres && bash scripts/test_drop.sh"
