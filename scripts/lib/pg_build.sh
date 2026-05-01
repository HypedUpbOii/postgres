#!/bin/bash
# In-container: configure + make + install PostgreSQL.
#
# --enable-debug only adds -g symbols; keeps -O2 (fine for benchmarks).
# --enable-cassert adds runtime assertion checks (~15-30% overhead) —
# keep on during development; remove for final benchmark numbers.
set -e

cd /postgres
./configure \
    --prefix=/usr/local/pgsql \
    --enable-debug \
    --enable-cassert \
    --with-llvm \
    --with-lz4 \
    --with-zstd \
    --with-liburing

make -j"$(nproc)"
make install
