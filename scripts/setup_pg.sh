#!/bin/bash
set -e

CONTAINER_NAME="pg-dev"

echo "[*] Building PostgreSQL..."

docker compose exec $CONTAINER_NAME bash -c "
cd /postgres && \
bash scripts/build.sh
"

echo "[*] Installing auto_index extension..."

docker compose exec $CONTAINER_NAME bash -c "
cd /postgres && \
make -C contrib/auto_index install
"

echo "[*] Initializing and starting PostgreSQL..."

docker compose exec $CONTAINER_NAME bash -c "
cd /postgres && \
bash scripts/run.sh
"

echo "[*] Enabling shared_preload_libraries..."

docker compose exec $CONTAINER_NAME bash -c "
cd /postgres && \
grep -q \"shared_preload_libraries = 'auto_index'\" data/postgresql.conf || \
echo \"shared_preload_libraries = 'auto_index'\" >> data/postgresql.conf
"

echo "[*] Restarting PostgreSQL..."

docker compose exec $CONTAINER_NAME bash -c "
cd /postgres && \
pg_ctl -D data restart -l logfile -w
"

echo "[*] PostgreSQL + auto_index setup complete."