#!/bin/bash
set -e

echo "[*] Starting Docker container..."

docker compose up -d --build

echo "[*] Docker container is ready."