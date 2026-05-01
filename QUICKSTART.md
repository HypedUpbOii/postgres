# auto_index — Quickstart

This is a from-scratch setup guide for someone who has just cloned
this repository and wants to get all four benchmarks running. The
extension is `auto_index`: a PostgreSQL contrib module that watches
the live workload and automatically creates and drops indexes
(singleton or composite up to three columns).

If you want the technical writeup instead, see
[`report/report.pdf`](report/report.pdf) (architecture + per-strategy
benchmarks) and
[`report/submission_report.pdf`](report/submission_report.pdf)
(submission report with AI-usage disclosure).

---

## Prerequisites

Just one thing on the host:

- **Docker Engine 20+** with the **Compose v2** plugin
  (`docker compose ...`, not the legacy `docker-compose`).
  <https://docs.docker.com/get-docker/>

Everything else — the C toolchain, PostgreSQL build deps,
`tpch-dbgen`, `pgbench` — is installed inside the container.

The container runs as a non-root user with **UID 1000**, which
matches the typical first user on a Linux host. If your host UID is
different, files written by the container may end up owned by a UID
that doesn't exist on your host. The bind-mounted `data/` will still
work; only file ownership reports look odd.

Disk: budget about **10 GB free** for the build (PostgreSQL source +
build artefacts + the dev image + a SF=0.1 TPC-H dataset). RAM: 8 GB
is comfortable; 4 GB works for everything except SF=1 TPC-H.

---

## One command

```bash
scripts/quickstart.sh
```

That's it. The script walks the full pipeline:

1. Builds the Docker image and starts the container.
2. Builds and installs PostgreSQL inside the container.
3. Initialises the data directory and starts the server.
4. Builds and loads the `auto_index` extension.
5. Runs a 30-second pgbench smoke test that demonstrates the live
   ~50× TPS jump when `auto_index` creates the index mid-run.

Total time on a modern laptop: **~6 minutes** the first time
(dominated by `make -j` of PostgreSQL itself), seconds on subsequent
runs.

If anything goes wrong, the script prints a clear error and an
`scripts/run.sh logs` hint so you can see what the server thinks.

---

## What you get

After `quickstart.sh` finishes, the container `postgres-dev` is
running with the extension loaded. To poke at it:

```bash
scripts/run.sh psql                                       # interactive shell
scripts/run.sh psql -c "SELECT * FROM auto_index_catalog" # which indexes did auto_index create?
scripts/run.sh logs                                       # tail the server log
scripts/run.sh shell                                      # bash inside the container
```

The four scripted benchmarks (each ~30 seconds to a few minutes):

```bash
scripts/benchmark.sh pgbench    # live 52x TPS step in a single run
scripts/benchmark.sh tpch       # 8-query 3-phase TPC-H (auto-runs setup)
scripts/benchmark.sh tpcc       # OLTP — confirms it doesn't over-index
scripts/demo.sh                 # full create-then-drop lifecycle (~90 s)
```

A side-by-side comparison of all six create-strategies on TPC-H:

```bash
scripts/benchmark.sh strategies
```

---

## Step-by-step (if you'd rather not run the bootstrap)

If you'd rather walk the steps manually — useful when something
breaks — here's what `quickstart.sh` does, broken out:

### 1. Bring up the container

```bash
scripts/build.sh docker
```

Idempotent. Builds the Ubuntu 22.04 image with all the C
build-deps, starts a container named `postgres-dev`, and bind-mounts
the repository at `/postgres` inside the container so edits on your
host show up immediately.

### 2. Build PostgreSQL

```bash
scripts/build.sh pg
```

Runs `./configure --prefix=/usr/local/pgsql --enable-debug
--enable-cassert --with-llvm --with-lz4 --with-zstd --with-liburing`,
`make -j$(nproc)`, and `make install` inside the container. ~3–5
minutes the first time. Incremental rebuilds (after editing C code)
are seconds.

### 3. Initialise the data directory and start the server

```bash
scripts/build.sh init
```

Runs `initdb` into `/postgres/data/` (which lives on your host via
the bind mount), wires `shared_preload_libraries = 'auto_index'` into
`postgresql.conf`, and starts the server.

### 4. Build and load the extension

```bash
scripts/build.sh ext
```

Compiles `contrib/auto_index/`, copies the SQL into the install
directory, restarts the server, and `DROP EXTENSION IF EXISTS / CREATE
EXTENSION` to refresh the catalog. Run this every time you edit C or
SQL inside `contrib/auto_index/`.

### 5. Run a benchmark

```bash
scripts/benchmark.sh pgbench
```

The pgbench live demo is the easiest sanity check — you'll see TPS
sit around 300–400 for the first 4–6 seconds, then jump to ~17 000
in a single window when `auto_index` creates the index. If that
works, everything else works.

---

## Common environment-variable tunables

All benchmarks accept these env vars (defaults shown):

| Var                  | Default | What it controls |
|---|---|---|
| `ITERATIONS`         | 1       | per-query repetitions (median taken) |
| `CHECK_INTERVAL`     | 5       | bgworker wake interval, seconds |
| `THRESHOLD`          | 2       | `auto_index.threshold` |
| `WAIT_FOR_BGWORKER`  | 20      | seconds to wait after the training phase |
| `TRAINING_PASSES`    | 3       | query passes during the training phase |

Per-benchmark:

| Var                   | Applies to | Default |
|---|---|---|
| `ROWS`, `DURATION`, `CLIENTS`, `JOBS`, `PROGRESS` | pgbench | 300000 / 20s / 4 / 2 / 2s |
| `SF`                  | tpch    | 0.1 (try 1 for the real result) |
| `STRATEGY`            | tpcc / strategies | `ratio_only` / all six |

Examples:

```bash
SF=1 scripts/benchmark.sh tpch                    # full SF=1 run, ~6 M lineitem rows
SCALE=4 DURATION_S=60 scripts/benchmark.sh tpcc   # bigger TPC-C
ROWS=1000000 scripts/benchmark.sh pgbench         # bigger pgbench table
```

---

## When things go wrong

**Symptom: `pg_ctl: command not found` after `docker compose up -d`.**
The PostgreSQL install lives on a named Docker volume
(`pg_install`) — but if that volume gets cleared (e.g. you ran
`docker volume prune`), you'll need to re-run `scripts/build.sh pg`
and `scripts/build.sh ext` to repopulate it. Data on the bind mount
survives.

**Symptom: TPC-H query fails with `could not resize shared memory
segment`.** Docker's default `/dev/shm` is 64 MB. Our
`docker-compose.yaml` already raises this to 1 GB via `shm_size: 1g`,
but if you have an *old* container from a previous compose file,
`docker compose down && docker compose up -d` to recreate it. (The
named `pg_install` volume means this no longer destroys the PG
install.)

**Symptom: `lineitem has N rows` doesn't match the SF you asked for.**
TPC-H setup is idempotent — once `lineitem` has data, it skips
loading. To force a reload:

```bash
SF=0.1 scripts/benchmark.sh tpch setup    # drop + recreate + COPY
```

**Symptom: bgworker isn't doing anything.** Check the log:

```bash
scripts/run.sh logs
```

If you see `auto_index: bgworker not configured`, the extension
isn't in `shared_preload_libraries` — re-run `scripts/build.sh init`
or add it manually:

```bash
scripts/run.sh psql -c "ALTER SYSTEM SET shared_preload_libraries = 'auto_index'"
scripts/run.sh restart
```

---

## Tearing down

Stop the server but keep the container:

```bash
scripts/run.sh stop
```

Stop and remove the container (data, source, and the named PG-install
volume persist):

```bash
docker compose down
```

Wipe everything including the PG-install volume (you'll need to
`scripts/build.sh pg` again to bring it back):

```bash
docker compose down -v
```

---

## Where things live

```
.
├── Dockerfile, docker-compose.yaml
├── scripts/
│   ├── quickstart.sh          ← you ran this
│   ├── build.sh               ← lifecycle (docker / pg / init / ext / clean / all)
│   ├── run.sh                 ← manual ops (start / stop / psql / logs / shell)
│   ├── benchmark.sh           ← four named benchmarks + strategies
│   ├── demo.sh                ← create-then-drop lifecycle
│   ├── build_submission.sh    ← assembles the submission/ folder
│   ├── lib/                   ← in-container helpers called by the entries
│   ├── tpch_queries/          ← 8 representative TPC-H queries
│   ├── tpcc_xn/               ← TPC-C-lite transactions
│   └── *_schema.sql, *_load.sql
├── contrib/auto_index/        ← the extension itself (1.8 KLoC C + SQL)
├── data/                      ← PostgreSQL data dir (bind-mounted into the container)
├── report/                    ← markdown + LaTeX + PDFs
├── submission/                ← packaged deliverable (regenerate with build_submission.sh)
├── setup.md                   ← deeper walkthrough of every script
└── (rest of the upstream PostgreSQL source tree)
```

For everything beyond the quickstart — alternate workflows, every
flag of every script, what each `lib/` helper does, how to add a new
strategy — see [`setup.md`](setup.md).
