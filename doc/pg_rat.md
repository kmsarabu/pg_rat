# pg_rat Technical Documentation

## Architecture Overview

`pg_rat` consists of six C source files, each handling a distinct responsibility:

```
pg_rat.c            → Module entry point, GUC definitions, hook wrappers, SQL functions
pg_rat_capture.c    → ExecutorEnd and ProcessUtility hook implementations
pg_rat_shmem.c      → Lock-free shared memory ring buffer (push/drain)
pg_rat_bgworker.c   → Background worker that flushes events to NDJSON files
pg_rat_replayer.c   → Workload replay coordinator with session mapping
pg_rat_spa.c        → SQL Performance Analyzer (EXPLAIN ANALYZE comparison)
pg_rat_report.c     → HTML regression report generator
```

### 1. Capture Engine (`pg_rat_capture.c`)

The capture engine hooks into PostgreSQL's execution pipeline:

*   **ExecutorEnd_hook:** Captures all DML (SELECT, INSERT, UPDATE, DELETE) after execution completes. Extracts timing from `instr_time`, row counts from `EState`, and buffer statistics from `BufferUsage`.
*   **ProcessUtility_hook:** Captures DDL (CREATE, DROP), transaction control (BEGIN, COMMIT, ROLLBACK), and utility commands (COPY, VACUUM, SET).
*   **Zero-Copy Design:** The hooks populate a small `RatEventMeta` struct (64 bytes) on the stack and pass it with a pointer to the query text directly to the ring buffer push function. No `palloc`, no `memset` of the full event, no disk I/O.
*   **Error Isolation:** The `ExecutorEnd` hook is wrapped in `PG_TRY/PG_CATCH` so a capture failure can never crash a user's query.

### 2. Lock-Free Ring Buffer (`pg_rat_shmem.c`)

The ring buffer is the core data structure enabling low-overhead capture:

*   **Atomic Slot Reservation:** Backends use `pg_atomic_compare_exchange_u64` on the `head` pointer to claim a slot index. Multiple backends can claim slots simultaneously without blocking each other.
*   **Slot State Machine:** Each slot has a `status` field with three states:
    - `EMPTY` → Available for a backend to claim
    - `BUSY` → A backend is writing data into this slot
    - `READY` → Data is complete; the flusher can drain this slot
*   **Bounded Spin:** If a claimed slot is still not `EMPTY` (the flusher hasn't cleared it yet), the backend spins with `SPIN_DELAY()` for up to 10,000 iterations, then drops the event. This prevents infinite hangs if the flusher crashes.
*   **Sequential Draining:** The flusher reads slots starting from `tail`, only draining `READY` slots, and advances `tail` atomically after each batch.

### 3. Background Flusher (`pg_rat_bgworker.c`)

A registered `BackgroundWorker` runs as a separate process:

*   **Wake Cycle:** Wakes every 100ms via `WaitLatch` with `WL_TIMEOUT`.
*   **Batched Draining:** Drains up to 1,024 events per cycle into a local buffer, then serializes each event to NDJSON format.
*   **JSON Escaping:** Uses a hand-written escape function for control characters, avoiding the overhead of a full JSON library.
*   **File Rotation:** Automatically rotates output files at 64 MB to prevent massive file handles and enable parallel processing.
*   **Graceful Shutdown:** On `SIGTERM`, drains remaining events before exiting.

### 4. Replay Coordinator (`pg_rat_replayer.c`)

Launched as a dynamic background worker by `pg_rat_start_replay()`:

*   **Session Mapping:** Pre-scans all NDJSON files to build a unique session map. Opens one `PGconn` per unique `session_id` (up to 1,024 sessions).
*   **Timing Fidelity:** Sorts all events by timestamp, then sleeps until the original relative offset from capture start is reached before dispatching each query.
*   **Error Tracking:** Counts failed queries via atomic counters, accessible through `pg_rat_replay_status()`.

### 5. SQL Performance Analyzer (`pg_rat_spa.c`)

Invoked by `pg_rat_run_spa()`:

*   **Deduplication:** Uses a hash table (`HTAB`) to collate unique queries by a djb2 hash of the query text. Tracks execution count and average source duration.
*   **EXPLAIN ANALYZE:** For each unique query, runs `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` via SPI, extracting execution time and buffer statistics from the JSON output.
*   **Regression Calculation:** Computes `regression_pct = ((target_ms - source_ms) / source_ms) * 100`. Queries with >10% regression are flagged.
*   **Results Storage:** Inserts comparison data into `pg_rat_spa_results` table using parameterized SPI queries (safe from SQL injection).

### 6. HTML Report Generator (`pg_rat_report.c`)

Invoked by `pg_rat_generate_report()`:

*   **Dark-Mode Design:** Uses CSS custom properties with a modern color palette (slate/blue/green/red).
*   **Summary Cards:** Displays total queries, regressions (>10%), improvements (>10%), and unchanged counts.
*   **Sortable Table:** Client-side JavaScript enables column sorting by clicking headers.
*   **Responsive:** Mobile-friendly layout with media queries.

## GUC Parameters

| Parameter | Default | Context | Description |
|:---|:---|:---|:---|
| `pg_rat.enabled` | `on` | SUSET | Global switch to enable/disable capture hooks. |
| `pg_rat.ring_buffer_size` | `8192` | POSTMASTER | Number of event slots in shared memory. Higher values handle bursty traffic. |
| `pg_rat.max_query_length` | `1024` | POSTMASTER | Maximum bytes of query text stored per event. Longer queries are truncated. |
| `pg_rat.capture_directory` | `pg_rat_capture` | SIGHUP | Directory for NDJSON output files, relative to `PGDATA`. |

## SQL Functions Reference

| Function | Returns | Description |
|:---|:---|:---|
| `pg_rat_start_capture(name text)` | `void` | Begin capturing workload into the named session. |
| `pg_rat_stop_capture()` | `void` | Stop the active capture and log summary. |
| `pg_rat_capture_status()` | `record` | Current capture state: active, name, events, drops, buffer size. |
| `pg_rat_export_capture(path text)` | `void` | Archive capture directory to a `.tar.gz` file. |
| `pg_rat_import_capture(path text)` | `void` | Extract a `.tar.gz` archive into the capture directory. |
| `pg_rat_start_replay(name text)` | `void` | Replay a captured workload with original timing. |
| `pg_rat_replay_status()` | `record` | Current replay progress: sessions, events done/total, errors. |
| `pg_rat_run_spa(name text)` | `void` | Run SQL Performance Analysis on a capture. |
| `pg_rat_generate_report(path text)` | `text` | Generate HTML report. Returns HTML or writes to `path`. |

## Performance Benchmark

### Test Environment
*   PostgreSQL 19devel on Ubuntu (Docker, ARM64)
*   `pgbench` Scale 1, 16 clients, 4 threads, 30-minute runs
*   `pg_rat.ring_buffer_size = 65536`

### Results

| Scenario | TPS | Latency (avg) | vs Baseline |
|:---|---:|---:|:---|
| **True Baseline** (No extension) | 2,797 | 5.72 ms | — |
| **RAT Capture** (to disk) | 2,594 | 6.17 ms | −7.2% |

### Interpretation

*   **7.2% is the total end-to-end overhead** in a sustained, worst-case stress test.
*   In real-world workloads where statement execution time is significantly longer than the fixed hook cost, overhead is expected to be much lower (< 1%).

For detailed visual profiling and system call analysis, see [PERFORMANCE.md](PERFORMANCE.md).

## Internal Data Format

Workload files (`.ndjson`) use one JSON object per line:

```json
{"ts":1714286400000000,"sid":"a1b2c3d4","xid":1234,"type":"QUERY_END","dur_ms":1.250,"rows":10,"shblk_hit":5,"shblk_read":0,"qlen":35,"query":"SELECT * FROM users WHERE id = 1"}
```

| Field | Type | Description |
|:---|:---|:---|
| `ts` | int64 | Microseconds since Unix Epoch (PostgreSQL `TimestampTz`) |
| `sid` | hex string | Session ID — hash of PID + session start time |
| `xid` | uint32 | Local virtual transaction ID |
| `type` | string | `QUERY_END`, `UTILITY`, `TXN_BEGIN`, `TXN_COMMIT`, `TXN_ROLLBACK` |
| `dur_ms` | float | Execution duration in milliseconds |
| `rows` | int64 | Rows affected or returned |
| `shblk_hit` | int64 | Shared buffer hits |
| `shblk_read` | int64 | Shared buffer disk reads |
| `qlen` | int | Length of query text (before truncation) |
| `query` | string | SQL query text (JSON-escaped, truncated to `max_query_length`) |

## Production Tuning Guide

### High-TPS Workloads (> 10,000 TPS)
```ini
pg_rat.ring_buffer_size = 262144    # 256K slots
```
Monitor `SELECT dropped_events FROM pg_rat_capture_status()`. If drops are high, increase the buffer or use a faster disk.

### Dedicated I/O
Point capture output to a separate physical disk or NVMe SSD:
```ini
pg_rat.capture_directory = '/mnt/fast_ssd/pg_rat_capture'
```
This prevents the flusher from competing with WAL writes.

### Minimal Overhead Mode
To capture only the query text without buffer statistics (useful when you only need the replay data, not SPA metrics), set:
```ini
pg_rat.enabled = off    # Disable capture temporarily
```

### Export & Import Between Servers
```sql
-- On source server: archive the capture
SELECT pg_rat_export_capture('/tmp/workload_v1.tar.gz');

-- On target server: import and replay
SELECT pg_rat_import_capture('/tmp/workload_v1.tar.gz');
SELECT pg_rat_start_replay('workload_v1');
```
