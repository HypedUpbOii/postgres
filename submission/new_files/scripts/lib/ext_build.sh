#!/bin/bash
# In-container: rebuild the auto_index extension and reload it.
# Use this after editing auto_index.c / .h / .sql.
set -e

export PATH=/usr/local/pgsql/bin:$PATH
cd /postgres

make -C contrib/auto_index install

# Force-update installed SQL — `make install` of pgxs sometimes uses
# whatever's cached.  Copy the source .sql to make sure the latest
# helpers (e.g. auto_index_attnames) are picked up.
cp contrib/auto_index/auto_index--1.0.sql /usr/local/pgsql/share/extension/

# First-time only: enable the bgworker by adding auto_index to
# shared_preload_libraries.  Done here (not in pg_init.sh) so the .so
# is installed before the server tries to load it on next start.
DATA=/postgres/data
if [ -d "$DATA" ] && \
   ! grep -q "shared_preload_libraries.*auto_index" "$DATA/postgresql.conf"; then
    echo "shared_preload_libraries = 'auto_index'" >> "$DATA/postgresql.conf"
fi

pg_ctl -D /postgres/data restart -l /postgres/logfile -w -s

# DROP + CREATE so the catalog table picks up any schema changes.
psql -d postgres <<'SQL' > /dev/null
DROP EXTENSION IF EXISTS auto_index CASCADE;
CREATE EXTENSION auto_index;
SQL

echo "✓ Extension built and reloaded."
