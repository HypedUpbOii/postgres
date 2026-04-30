# Development Setup

This project hacks on PostgreSQL internals inside a Docker container so your host
system stays clean and everyone gets an identical build environment.

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/) (Engine 20+ or Docker Desktop)
- [Docker Compose](https://docs.docker.com/compose/install/) (v2 — `docker compose`, not `docker-compose`)

No other dependencies are required on your host machine.

## Directory layout

```
.
├── Dockerfile           # Ubuntu 22.04 image with all build deps
├── docker-compose.yaml  # Dev container definition
├── scripts/
│   ├── build.sh         # configure + make + make install
│   └── run.sh           # initdb (first run only) + pg_ctl start
├── plan/
│   ├── files_to_add.md      # new files for the auto_index extension
│   └── files_to_modify.md   # config changes; no core source edits needed
└── src/                 # PostgreSQL source tree
```

## First-time setup

### 1. Build the Docker image

```bash
docker compose build
```

This installs all compile-time dependencies (OpenSSL, ICU, libxml2, etc.) into the
image. You only need to re-run this if `Dockerfile` changes.

### 2. Start the container

```bash
docker compose up -d
```

The container mounts the whole repository at `/postgres` inside it, so any edits you
make on your host are immediately visible inside the container and vice versa.

### 3. Open a shell inside the container

```bash
docker compose exec pg-dev bash
```

All subsequent commands in this guide are run **inside this shell**.

## Building PostgreSQL

```bash
bash scripts/build.sh
```

This runs `./configure`, `make -j$(nproc)`, and `make install` with the prefix
`/usr/local/pgsql`. On a modern machine with 8 cores the full build takes roughly
3–5 minutes. Incremental rebuilds after editing a single `.c` file take a few seconds.

After the script finishes, the installed binaries (`psql`, `pg_ctl`, `initdb`, etc.)
are at `/usr/local/pgsql/bin/`, which is already on `PATH` inside the container.

## Cleaning the build

Remove compiled objects but keep the configured build system (fast incremental rebuild):
```bash
make clean
```

Wipe everything including `configure` output back to a pristine state (required if
switching branches or fixing permission issues from a previous root build):
```bash
make distclean
```

## Starting a database instance

```bash
bash scripts/run.sh
```

On the first run this initialises a data directory at `./data/` and starts the server.
On subsequent runs it skips `initdb` and just starts the server.

Logs are written to `./logfile` in the repo root.

## Connecting with psql

```bash
psql -U $(whoami) postgres
```

Or connect as the default superuser created by `initdb`:

```bash
psql -U root postgres
```

## Stopping the server

```bash
pg_ctl -D data stop
```

## Rebuilding after source changes

For changes to a single file:

```bash
make -C src/backend          # or whichever subdirectory changed
make install
```

For changes that touch headers or affect many files, do a full rebuild:

```bash
bash scripts/build.sh
```

You will need to restart the server after installing new binaries:

```bash
pg_ctl -D data restart -l logfile
```

## Working on the auto_index extension

The extension will live at `contrib/auto_index/`. Once the files are in place:

```bash
# Build and install the extension shared library
make -C contrib/auto_index install

# Add to postgresql.conf so hooks load at startup
echo "shared_preload_libraries = 'auto_index'" >> data/postgresql.conf

# Restart for the GUC and shared memory registration to take effect
pg_ctl -D data restart -l logfile

# Install SQL objects into your database
psql -U root postgres -c "CREATE EXTENSION auto_index;"
```

To verify the extension loaded:

```bash
psql -U root postgres -c "SELECT * FROM auto_index_stats();"
```

## Stopping and removing the container

```bash
docker compose down
```

This stops and removes the container but leaves the image intact. Your source edits,
the `data/` directory, and `logfile` all persist on your host because they are part
of the mounted volume.

To also remove the image:

```bash
docker compose down --rmi local
```
