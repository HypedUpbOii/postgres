#!/bin/bash
#
# Run TPC-H setup (clone dbgen, generate data, load schema) inside the
# running container.  Forwards SF and FORCE_RELOAD env vars.
#
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Running TPC-H setup..."

docker compose exec \
    -e SF \
    -e FORCE_RELOAD \
    -e DBGEN_REPO \
    $CONTAINER_NAME bash -c "cd /postgres && bash scripts/setup_tpch.sh"
