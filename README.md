# :rat: pg_rat: Real Application Testing for PostgreSQL

`pg_rat` is a high-performance PostgreSQL extension for **Real Application Testing (RAT)** and **SQL Performance Analysis (SPA)**. It captures production workloads with minimal overhead, replays them with original concurrency and timing on a target system, and generates comprehensive HTML regression reports.

Inspired by Oracle RAT, `pg_rat` is designed to de-risk database migrations, major version upgrades, and hardware changes by validating performance at scale before going live.

## 🚀 Key Features

*   **Lock-Free Workload Capture:** Uses an atomic compare-exchange shared memory ring buffer and a background flusher to capture SQL queries (DDL, DML, and Utility) with minimal performance impact.
*   **Workload Replay:** Native replay coordinator that preserves session mapping and relative event timing on a best-effort basis (full asynchronous concurrent replay is planned).
*   **SQL Performance Analyzer (SPA):** Automated per-query analysis comparing source metrics (captured) against target performance (`EXPLAIN ANALYZE`).
*   **Visual Reporting:** Generates modern, dark-mode HTML reports with statistical summaries of regressions, improvements, and execution plan changes.
*   **NDJSON Storage:** Workloads are stored in human-readable, line-delimited JSON format for easy analysis or transformation.

## ⚡ Performance

`pg_rat` is built for production environments where performance is non-negotiable.

### Capture Overhead Summary

In a worst-case `pgbench` scale-1 workload, where each transaction is very small and PostgreSQL executes thousands of short statements per second, pg_rat capture introduced approximately 7% throughput overhead.

| Test | TPS | Avg Latency | vs Baseline |
|---|---:|---:|---:|
| **True Baseline** (No extension) | 2,797 | 5.72 ms | — |
| **RAT Capture Active** | 2,594 | 6.17 ms | ~7.2% |

This benchmark intentionally stresses the capture path with very short statements. For longer-running application queries, the fixed per-statement capture cost is expected to be a much smaller percentage of total query latency, but users should benchmark with their own workload before enabling production capture.

### 🔍 Deep Dive: Performance & Flamegraphs

For detailed visual profiling and system call analysis, see the [Performance Deep Dive](doc/PERFORMANCE.md).

### Why Events Are Dropped

The benchmark intentionally prioritizes database latency over capture completeness. Dropped events indicate the flusher or storage path could not keep up with event production. This is expected under extreme pgbench scale-1 pressure, but for production RAT use, dropped events should be monitored and minimized by:
*   Using a dedicated fast SSD for `pg_rat.capture_directory`
*   Increasing `pg_rat.ring_buffer_size` for bursty workloads
*   Reducing capture scope where possible

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
