#!/bin/bash
set -e

export PATH=/usr/local/pgsql/bin:$PATH

make -C contrib/auto_index install
pg_ctl -D data restart -l logfile
