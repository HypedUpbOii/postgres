#!/bin/bash
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Running auto_index benchmark..."

docker compose exec $CONTAINER_NAME bash -c "
cd /postgres && \
bash scripts/test_auto_index.sh
"