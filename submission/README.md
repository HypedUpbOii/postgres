# auto_index — Project Submission

PostgreSQL extension that observes live workload and automatically
creates / drops indexes (singleton or composite up to 3 columns).

**Public git repository (forked from `postgres/postgres`):**
<https://github.com/HypedUpbOii/postgres>

The link is also in `git_url.txt`. The repo is a fork of upstream
PostgreSQL: the merge-base ancestor commit is preserved, and all our
work is on top.

## What's in this folder

| Path | Contents |
|---|---|
| `report/submission_report.pdf` | This submission report (motivation, functionality, code, testing, AI usage). |
| `report/report.pdf` | Full technical report (architecture deep-dive + per-strategy benchmark numbers). |
| `diffs/` | Per-file diffs vs. the upstream PostgreSQL merge-base. The *only* upstream file we modified is `.gitignore` — every other change is a new file, listed under `new_files/` instead. |
| `new_files/` | Every file we authored, mirrored at its original repository path so the layout matches the live repo. |
| `test_data/` | TPC-H queries (Q1, Q3, Q5, Q6, Q10, Q12, Q14, Q19), TPC-H schema, TPC-C-lite schema + load + four transactions. The TPC-H raw `.tbl` data (~70 MB at SF=0.1) is *not* shipped — it is regenerated on demand by `scripts/lib/tpch_setup.sh` via `tpch-dbgen`. |

## Reproducing the results

The fastest path from a clean clone to all four headline benchmarks is:

```bash
git clone https://github.com/HypedUpbOii/postgres.git
cd postgres
scripts/quickstart.sh        # builds container, PG, extension, then runs a smoke pgbench demo
```

From there, the four named benchmarks individually:

```bash
scripts/benchmark.sh pgbench    # ~52x live step in TPS
scripts/benchmark.sh tpch       # 8-query 3-phase TPC-H (auto-runs setup)
scripts/benchmark.sh tpcc       # TPC-C-lite OLTP
scripts/demo.sh                 # full create-then-drop lifecycle (~90 s)
```

See `QUICKSTART.md` in the repository root for a detailed
from-scratch walkthrough.
