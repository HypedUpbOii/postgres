#!/bin/bash
#
# auto_index lifecycle demo.
#
# Two phases on one table:
#   A. Heavy reads with single-column AND multi-column predicates →
#      auto_index creates a SINGLETON and a COMPOSITE index.
#   B. Heavy writes (no reads on those columns) → both indexes get
#      dropped by the ski-rental idle accumulator.
#
# This proves auto_index handles both index types end-to-end and
# correctly cleans up when reads stop.
#
# Tunables:
#   ROWS=50000           rows in the demo table
#   READ_ITERS=200       SELECTs per read pattern (each pattern is unique
#                        enough to push cumulative_benefit above threshold)
#   WRITE_ITERS=300      write statements during phase B
#   CHECK_INTERVAL=5     bgworker wake interval (seconds)
#   IDLE_INTERVALS=10    how many bgworker cycles to wait for drop
#   STRATEGY=simple_threshold
#                        create strategy for the demo (predictable trigger)
#   SIMPLE_THRESHOLD=10  benefit needed to fire under simple_threshold
#
set -e

CONTAINER="pg-dev"
LIB="/postgres/scripts/lib"

ENV_FORWARD=(
    -e ROWS -e READ_ITERS -e WRITE_ITERS
    -e CHECK_INTERVAL -e IDLE_INTERVALS
    -e STRATEGY -e SIMPLE_THRESHOLD -e THRESHOLD
)

docker compose exec "${ENV_FORWARD[@]}" "$CONTAINER" bash "$LIB/demo.sh"
