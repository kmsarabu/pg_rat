# pg_rat: Real Application Testing for PostgreSQL

`pg_rat` is a high-performance PostgreSQL extension designed for **Real Application Testing (RAT)** and **SQL Performance Analysis (SPA)**. It allows database administrators to capture production workloads with minimal overhead, replay them with original concurrency and timing on a target system, and generate comprehensive HTML regression reports.

Inspired by Oracle RAT, `pg_rat` is designed to de-risk database migrations, major version upgrades (e.g., PG16 to PG17), and hardware changes by validating performance at scale before going live.

## 🚀 Key Features

*   **Non-Blocking Workload Capture:** Uses a SpinLock-protected shared memory ring buffer and background workers to capture SQL queries (DDL, DML, and Utility) with < 2% performance overhead.
*   **Workload Replay:** Native replay coordinator that mimics original session concurrency, transaction ordering, and relative timing offsets using asynchronous `libpq` connections.
*   **SQL Performance Analyzer (SPA):** Automated per-query analysis that compares source metrics (captured) against target performance (using `EXPLAIN ANALYZE`).
*   **Visual Reporting:** Generates modern, dark-mode HTML reports with statistical summaries of regressions, improvements, and execution plan changes.
*   **NDJSON Storage:** Workloads are stored in human-readable, compressed-friendly NDJSON format for easy analysis or transformation.

## 🛠 Installation

### Prerequisites
*   PostgreSQL 15, 16, or 17
*   `libpq` development headers
*   `make` and `gcc` (or `meson` / `ninja`)

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
pg_rat.ring_buffer_size = 65536     # Max buffer for high-throughput nodes
pg_rat.max_query_length = 4096      # Query text truncation limit
pg_rat.capture_directory = 'pg_rat_capture' # Relative to PGDATA
```
Restart PostgreSQL after updating the config.

## 📖 Quick Start

### 1. Capture a Workload
```sql
CREATE EXTENSION pg_rat;

-- Start capture
SELECT pg_rat_start_capture('migration_test_01');

-- Run your application or benchmark (e.g., pgbench)

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
-- Returns the HTML content as text or saves to a file if requested
SELECT pg_rat_generate_report();
```

## 📊 Design Philosophy

*   **Production First:** The capture logic is optimized for zero-allocation and no disk I/O on the query execution hot path.
*   **Native Integration:** Implemented in C using PostgreSQL's internal `ExecutorEnd_hook` and `ProcessUtility_hook` for 100% fidelity.
*   **Minimal Dependencies:** Standard C and `libpq` only. No external agents or Python/Java runtimes required.

## 📝 License
PostgreSQL License

