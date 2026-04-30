#!/bin/bash
set -e

# --enable-debug only adds -g symbols; it keeps -O2 and is fine for benchmarking.
# --enable-cassert adds runtime assertion checks (~15-30% overhead).
# Remove --enable-cassert for final benchmark runs, keep it during development.
./configure \
  --prefix=/usr/local/pgsql \
  --enable-debug \
  --enable-cassert \
  --with-llvm \
  --with-lz4 \
  --with-zstd \
  --with-liburing

make -j$(nproc)
make install