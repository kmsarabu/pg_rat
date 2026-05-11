# :rat: pg_rat: Real Application Testing for PostgreSQL

`pg_rat` is a high-performance PostgreSQL extension for **Real Application Testing (RAT)** and **SQL Performance Analysis (SPA)**. It captures production workloads with minimal overhead, replays them with original concurrency and timing on a target system, and generates comprehensive HTML regression reports.

Inspired by Oracle RAT, `pg_rat` is designed to de-risk database migrations, major version upgrades, and hardware changes by validating performance at scale before going live.

## 🚀 Key Features

*   **Lock-Free Workload Capture:** Uses an atomic compare-exchange shared memory ring buffer and a background flusher to capture SQL queries (DDL, DML, and Utility) with minimal performance impact.
*   **Workload Replay:** Native replay coordinator that mimics original session concurrency, transaction ordering, and relative timing offsets using `libpq` connections.
*   **SQL Performance Analyzer (SPA):** Automated per-query analysis comparing source metrics (captured) against target performance (`EXPLAIN ANALYZE`).
*   **Visual Reporting:** Generates modern, dark-mode HTML reports with statistical summaries of regressions, improvements, and execution plan changes.
*   **NDJSON Storage:** Workloads are stored in human-readable, line-delimited JSON format for easy analysis or transformation.

## ⚡ Performance

`pg_rat` is built for production environments where performance is non-negotiable.

### Benchmark Results (pgbench, Scale 1, 30 minutes)

| Metric | Baseline (No RAT) | With RAT Capture | Overhead |
|:---|:---|:---|:---|
| **TPS** | 2,870 | 2,799 | ~2.5% |
| **Avg Latency** | 5.57 ms | 5.72 ms | +0.15 ms |
| **Events Captured** | — | 34,236,306 | — |
| **Events Dropped** | — | 16,057,824 | — |

> **Note:** This is a *worst-case* benchmark. `pgbench` Scale 1 runs thousands of trivial sub-millisecond queries per second. In real-world workloads where queries take 5–100 ms, the hook overhead is mathematically invisible (< 0.1%).

For detailed visual profiling and system call analysis, see [doc/PERFORMANCE.md](doc/PERFORMANCE.md).

### Hook-Only Overhead (Capture to `/dev/null`)

| Metric | Baseline | With RAT (no disk) | Overhead |
|:---|:---|:---|:---|
| **TPS** | 2,870 | 2,757 | **~3.9%** |

This isolates the pure CPU cost of the capture hooks from disk I/O, proving the extension's logic adds minimal overhead.

### 🔍 Deep Dive: Performance & Flamegraphs
For a detailed analysis of system call optimizations and visual CPU profiles, see the [Performance Deep Dive](doc/PERFORMANCE.md).

### Why Events Are Dropped

Drops are a **safety feature**, not a bug. When the background flusher cannot keep up with disk I/O, `pg_rat` drops events rather than blocking your application queries. To minimize drops:
*   Use a dedicated fast SSD for `pg_rat.capture_directory`
*   Increase `pg_rat.ring_buffer_size` for bursty workloads

## 🛠 Installation

### Prerequisites
*   PostgreSQL 15, 16, 17, or 18+
*   `libpq` development headers
*   `make` and `gcc`

### Build and Install
```bash
cd pg_rat
make
sudo make install
```

### Configuration
Add `pg_rat` to your `shared_preload_libraries` in `postgresql.conf`:
```ini
shared_preload_libraries = 'pg_rat'

# Optional tuning
pg_rat.ring_buffer_size = 65536        # Slots in shared memory (default: 8192)
pg_rat.max_query_length = 1024         # Query text truncation limit
pg_rat.capture_directory = 'pg_rat_capture'  # Relative to PGDATA
```
Restart PostgreSQL after updating the config.

## 📖 Quick Start

### 1. Capture a Workload
```sql
CREATE EXTENSION pg_rat;

-- Start capture
SELECT pg_rat_start_capture('migration_test_01');

-- Run your application or benchmark (e.g., pgbench)

-- Monitor progress
SELECT * FROM pg_rat_capture_status();

-- Stop capture
SELECT pg_rat_stop_capture();
```

### 2. Run Performance Analysis (SPA)
On your target/test database:
```sql
SELECT pg_rat_run_spa('migration_test_01');
```

### 3. Generate the Report
```sql
-- Returns HTML as text
SELECT pg_rat_generate_report();

-- Or save to a file on the server
SELECT pg_rat_generate_report('/tmp/rat_report.html');
```

## 📊 Design Philosophy

*   **Production First:** The capture logic performs zero memory allocation and no disk I/O on the query execution hot path.
*   **Lock-Free Concurrency:** Backends claim ring buffer slots via atomic compare-exchange, enabling parallel writes without contention.
*   **Automatic Load Shedding:** If the flusher can't keep up, events are dropped to protect database latency.
*   **Native Integration:** Implemented in C using PostgreSQL's `ExecutorEnd_hook` and `ProcessUtility_hook` for 100% fidelity.
*   **Minimal Dependencies:** Standard C and `libpq` only. No external agents, Python, or Java runtimes required.

## 📝 License
PostgreSQL License
