# Submission inventory

Upstream baseline: `3dd42ee97b812c0711b4d18e9cab069065367dca` (= `9a4faa1f02e4c13a578e5ef43a63c5350d72870f^`,
the last commit reachable from our fork that came from
upstream PostgreSQL).  Everything below is our work.

## Modified upstream files

| Original path | Diff file |
|---|---|
| `.gitignore` | [diffs/gitignore.diff](diffs/gitignore.diff) |

## New files we authored

Each appears below with both a `+`-only diff (in `diffs/`)
and the file itself (mirrored at its original path under
`new_files/`).

| Original path | Diff file | Mirrored at |
|---|---|---|
| `Dockerfile` | [diffs/Dockerfile.diff](diffs/Dockerfile.diff) | [new_files/Dockerfile](new_files/Dockerfile) |
| `QUICKSTART.md` | [diffs/QUICKSTART.md.diff](diffs/QUICKSTART.md.diff) | [new_files/QUICKSTART.md](new_files/QUICKSTART.md) |
| `contrib/auto_index/Makefile` | [diffs/contrib_auto_index_Makefile.diff](diffs/contrib_auto_index_Makefile.diff) | [new_files/contrib/auto_index/Makefile](new_files/contrib/auto_index/Makefile) |
| `contrib/auto_index/auto_index--1.0.sql` | [diffs/contrib_auto_index_auto_index--1.0.sql.diff](diffs/contrib_auto_index_auto_index--1.0.sql.diff) | [new_files/contrib/auto_index/auto_index--1.0.sql](new_files/contrib/auto_index/auto_index--1.0.sql) |
| `contrib/auto_index/auto_index.c` | [diffs/contrib_auto_index_auto_index.c.diff](diffs/contrib_auto_index_auto_index.c.diff) | [new_files/contrib/auto_index/auto_index.c](new_files/contrib/auto_index/auto_index.c) |
| `contrib/auto_index/auto_index.control` | [diffs/contrib_auto_index_auto_index.control.diff](diffs/contrib_auto_index_auto_index.control.diff) | [new_files/contrib/auto_index/auto_index.control](new_files/contrib/auto_index/auto_index.control) |
| `contrib/auto_index/auto_index.h` | [diffs/contrib_auto_index_auto_index.h.diff](diffs/contrib_auto_index_auto_index.h.diff) | [new_files/contrib/auto_index/auto_index.h](new_files/contrib/auto_index/auto_index.h) |
| `docker-compose.yaml` | [diffs/docker-compose.yaml.diff](diffs/docker-compose.yaml.diff) | [new_files/docker-compose.yaml](new_files/docker-compose.yaml) |
| `report/files_to_add.md` | [diffs/report_files_to_add.md.diff](diffs/report_files_to_add.md.diff) | [new_files/report/files_to_add.md](new_files/report/files_to_add.md) |
| `report/files_to_modify.md` | [diffs/report_files_to_modify.md.diff](diffs/report_files_to_modify.md.diff) | [new_files/report/files_to_modify.md](new_files/report/files_to_modify.md) |
| `report/postgres_internals.md` | [diffs/report_postgres_internals.md.diff](diffs/report_postgres_internals.md.diff) | [new_files/report/postgres_internals.md](new_files/report/postgres_internals.md) |
| `report/report.md` | [diffs/report_report.md.diff](diffs/report_report.md.diff) | [new_files/report/report.md](new_files/report/report.md) |
| `report/report.pdf` | [diffs/report_report.pdf.diff](diffs/report_report.pdf.diff) | [new_files/report/report.pdf](new_files/report/report.pdf) |
| `report/report.tex` | [diffs/report_report.tex.diff](diffs/report_report.tex.diff) | [new_files/report/report.tex](new_files/report/report.tex) |
| `report/submission_report.md` | [diffs/report_submission_report.md.diff](diffs/report_submission_report.md.diff) | [new_files/report/submission_report.md](new_files/report/submission_report.md) |
| `report/submission_report.pdf` | [diffs/report_submission_report.pdf.diff](diffs/report_submission_report.pdf.diff) | [new_files/report/submission_report.pdf](new_files/report/submission_report.pdf) |
| `report/submission_report.tex` | [diffs/report_submission_report.tex.diff](diffs/report_submission_report.tex.diff) | [new_files/report/submission_report.tex](new_files/report/submission_report.tex) |
| `scripts/benchmark.sh` | [diffs/scripts_benchmark.sh.diff](diffs/scripts_benchmark.sh.diff) | [new_files/scripts/benchmark.sh](new_files/scripts/benchmark.sh) |
| `scripts/build.sh` | [diffs/scripts_build.sh.diff](diffs/scripts_build.sh.diff) | [new_files/scripts/build.sh](new_files/scripts/build.sh) |
| `scripts/build_submission.sh` | [diffs/scripts_build_submission.sh.diff](diffs/scripts_build_submission.sh.diff) | [new_files/scripts/build_submission.sh](new_files/scripts/build_submission.sh) |
| `scripts/demo.sh` | [diffs/scripts_demo.sh.diff](diffs/scripts_demo.sh.diff) | [new_files/scripts/demo.sh](new_files/scripts/demo.sh) |
| `scripts/lib/bench_pgbench.sh` | [diffs/scripts_lib_bench_pgbench.sh.diff](diffs/scripts_lib_bench_pgbench.sh.diff) | [new_files/scripts/lib/bench_pgbench.sh](new_files/scripts/lib/bench_pgbench.sh) |
| `scripts/lib/bench_strategies.sh` | [diffs/scripts_lib_bench_strategies.sh.diff](diffs/scripts_lib_bench_strategies.sh.diff) | [new_files/scripts/lib/bench_strategies.sh](new_files/scripts/lib/bench_strategies.sh) |
| `scripts/lib/bench_tpcc.sh` | [diffs/scripts_lib_bench_tpcc.sh.diff](diffs/scripts_lib_bench_tpcc.sh.diff) | [new_files/scripts/lib/bench_tpcc.sh](new_files/scripts/lib/bench_tpcc.sh) |
| `scripts/lib/bench_tpch.sh` | [diffs/scripts_lib_bench_tpch.sh.diff](diffs/scripts_lib_bench_tpch.sh.diff) | [new_files/scripts/lib/bench_tpch.sh](new_files/scripts/lib/bench_tpch.sh) |
| `scripts/lib/clean.sh` | [diffs/scripts_lib_clean.sh.diff](diffs/scripts_lib_clean.sh.diff) | [new_files/scripts/lib/clean.sh](new_files/scripts/lib/clean.sh) |
| `scripts/lib/demo.sh` | [diffs/scripts_lib_demo.sh.diff](diffs/scripts_lib_demo.sh.diff) | [new_files/scripts/lib/demo.sh](new_files/scripts/lib/demo.sh) |
| `scripts/lib/ext_build.sh` | [diffs/scripts_lib_ext_build.sh.diff](diffs/scripts_lib_ext_build.sh.diff) | [new_files/scripts/lib/ext_build.sh](new_files/scripts/lib/ext_build.sh) |
| `scripts/lib/pg_build.sh` | [diffs/scripts_lib_pg_build.sh.diff](diffs/scripts_lib_pg_build.sh.diff) | [new_files/scripts/lib/pg_build.sh](new_files/scripts/lib/pg_build.sh) |
| `scripts/lib/pg_init.sh` | [diffs/scripts_lib_pg_init.sh.diff](diffs/scripts_lib_pg_init.sh.diff) | [new_files/scripts/lib/pg_init.sh](new_files/scripts/lib/pg_init.sh) |
| `scripts/lib/tpch_setup.sh` | [diffs/scripts_lib_tpch_setup.sh.diff](diffs/scripts_lib_tpch_setup.sh.diff) | [new_files/scripts/lib/tpch_setup.sh](new_files/scripts/lib/tpch_setup.sh) |
| `scripts/quickstart.sh` | [diffs/scripts_quickstart.sh.diff](diffs/scripts_quickstart.sh.diff) | [new_files/scripts/quickstart.sh](new_files/scripts/quickstart.sh) |
| `scripts/run.sh` | [diffs/scripts_run.sh.diff](diffs/scripts_run.sh.diff) | [new_files/scripts/run.sh](new_files/scripts/run.sh) |
| `scripts/tpcc_load.sql` | [diffs/scripts_tpcc_load.sql.diff](diffs/scripts_tpcc_load.sql.diff) | [new_files/scripts/tpcc_load.sql](new_files/scripts/tpcc_load.sql) |
| `scripts/tpcc_schema.sql` | [diffs/scripts_tpcc_schema.sql.diff](diffs/scripts_tpcc_schema.sql.diff) | [new_files/scripts/tpcc_schema.sql](new_files/scripts/tpcc_schema.sql) |
| `scripts/tpcc_xn/new_order.sql` | [diffs/scripts_tpcc_xn_new_order.sql.diff](diffs/scripts_tpcc_xn_new_order.sql.diff) | [new_files/scripts/tpcc_xn/new_order.sql](new_files/scripts/tpcc_xn/new_order.sql) |
| `scripts/tpcc_xn/order_status.sql` | [diffs/scripts_tpcc_xn_order_status.sql.diff](diffs/scripts_tpcc_xn_order_status.sql.diff) | [new_files/scripts/tpcc_xn/order_status.sql](new_files/scripts/tpcc_xn/order_status.sql) |
| `scripts/tpcc_xn/payment.sql` | [diffs/scripts_tpcc_xn_payment.sql.diff](diffs/scripts_tpcc_xn_payment.sql.diff) | [new_files/scripts/tpcc_xn/payment.sql](new_files/scripts/tpcc_xn/payment.sql) |
| `scripts/tpcc_xn/stock_level.sql` | [diffs/scripts_tpcc_xn_stock_level.sql.diff](diffs/scripts_tpcc_xn_stock_level.sql.diff) | [new_files/scripts/tpcc_xn/stock_level.sql](new_files/scripts/tpcc_xn/stock_level.sql) |
| `scripts/tpch_queries/q01.sql` | [diffs/scripts_tpch_queries_q01.sql.diff](diffs/scripts_tpch_queries_q01.sql.diff) | [new_files/scripts/tpch_queries/q01.sql](new_files/scripts/tpch_queries/q01.sql) |
| `scripts/tpch_queries/q03.sql` | [diffs/scripts_tpch_queries_q03.sql.diff](diffs/scripts_tpch_queries_q03.sql.diff) | [new_files/scripts/tpch_queries/q03.sql](new_files/scripts/tpch_queries/q03.sql) |
| `scripts/tpch_queries/q05.sql` | [diffs/scripts_tpch_queries_q05.sql.diff](diffs/scripts_tpch_queries_q05.sql.diff) | [new_files/scripts/tpch_queries/q05.sql](new_files/scripts/tpch_queries/q05.sql) |
| `scripts/tpch_queries/q06.sql` | [diffs/scripts_tpch_queries_q06.sql.diff](diffs/scripts_tpch_queries_q06.sql.diff) | [new_files/scripts/tpch_queries/q06.sql](new_files/scripts/tpch_queries/q06.sql) |
| `scripts/tpch_queries/q10.sql` | [diffs/scripts_tpch_queries_q10.sql.diff](diffs/scripts_tpch_queries_q10.sql.diff) | [new_files/scripts/tpch_queries/q10.sql](new_files/scripts/tpch_queries/q10.sql) |
| `scripts/tpch_queries/q12.sql` | [diffs/scripts_tpch_queries_q12.sql.diff](diffs/scripts_tpch_queries_q12.sql.diff) | [new_files/scripts/tpch_queries/q12.sql](new_files/scripts/tpch_queries/q12.sql) |
| `scripts/tpch_queries/q14.sql` | [diffs/scripts_tpch_queries_q14.sql.diff](diffs/scripts_tpch_queries_q14.sql.diff) | [new_files/scripts/tpch_queries/q14.sql](new_files/scripts/tpch_queries/q14.sql) |
| `scripts/tpch_queries/q19.sql` | [diffs/scripts_tpch_queries_q19.sql.diff](diffs/scripts_tpch_queries_q19.sql.diff) | [new_files/scripts/tpch_queries/q19.sql](new_files/scripts/tpch_queries/q19.sql) |
| `scripts/tpch_schema.sql` | [diffs/scripts_tpch_schema.sql.diff](diffs/scripts_tpch_schema.sql.diff) | [new_files/scripts/tpch_schema.sql](new_files/scripts/tpch_schema.sql) |
| `setup.md` | [diffs/setup.md.diff](diffs/setup.md.diff) | [new_files/setup.md](new_files/setup.md) |
