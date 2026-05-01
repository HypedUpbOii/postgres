#!/bin/bash
# In-container: initialise the data directory (first run) and start PG.
# Idempotent — safe to run multiple times.
set -e

export PATH=/usr/local/pgsql/bin:$PATH
DATA=/postgres/data

if [ ! -d "$DATA" ]; then
    initdb -D "$DATA" --locale=C --encoding=UTF8
fi

# Note: shared_preload_libraries='auto_index' is added by ext_build.sh
# AFTER the .so is built and installed.  Setting it here would make the
# server fail to start on first boot.
if ! pg_ctl -D "$DATA" status > /dev/null 2>&1; then
    pg_ctl -D "$DATA" start -l /postgres/logfile -w
fi

echo "PostgreSQL is up.  Run 'scripts/build.sh ext' to install auto_index."
