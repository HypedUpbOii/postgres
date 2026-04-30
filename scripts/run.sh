#!/bin/bash
set -e

export PATH=/usr/local/pgsql/bin:$PATH

if [ ! -d "data" ]; then
    initdb -D data
fi

pg_ctl -D data -l logfile start