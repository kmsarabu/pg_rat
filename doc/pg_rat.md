# pg_rat Technical Documentation

## Architecture Overview

### 1. Capture Engine (`pg_rat_capture.c`)
The capture engine hooks into the heart of the PostgreSQL execution pipeline:
*   **ExecutorEnd_hook:** Captures all DML (SELECT, INSERT, UPDATE, DELETE) at the end of execution. This provides finalized metrics for execution time, rows processed, and buffer hits/reads using the `instr_time` and `BufferUsage` frameworks.
*   **ProcessUtility_hook:** Captures DDL (CREATE, DROP), Transaction Control (BEGIN, COMMIT, ROLLBACK), and other utility commands (COPY, VACUUM).
*   **Minimal Impact:** The hooks only perform a single `memcpy` of a fixed-size `RatEvent` struct into a shared memory ring buffer. They do not write to disk or allocate memory.

### 2. Shared Memory & Ring Buffer (`pg_rat_shmem.c`)
Shared memory is used to bridge the gap between backends and the flusher.
*   **Ring Buffer:** A circular slot-based buffer. If the buffer is full, events are dropped to protect system stability, and a `dropped_events` counter is incremented.
*   **SpinLocks:** Used to protect the head/tail pointers. Hold time is extremely low (nanoseconds).

### 3. Background Flusher (`pg_rat_bgworker.c`)
A `BackgroundWorker` (BGW) runs as a separate process to asynchronously drain the ring buffer.
*   **Draining:** Wakes up every `pg_rat.naptime_ms` (default 100ms) or when signaled by a backend.
*   **NDJSON Formatting:** Event data is serialized to JSON string format and written to disk.
*   **File Rotation:** Automatically rotates logs when they reach 64MB to prevent massive file handles.

### 4. Replay Coordinator (`pg_rat_replayer.c`)
Uses a dynamic background worker to handle the replay logic.
*   **Concurrency:** Opens parallel `libpq` connections, mapping each `session_id` from the source to a persistent connection on the target.
*   **Timing:** Preserves relative inter-event gaps by sleeping until the original capture timestamp offset is reached.

### 5. SQL Performance Analyzer (`pg_rat_spa.c`)
Performs a comparative study.
*   **Deduplication:** Hash-collates unique queries from the NDJSON set so identical queries aren't run multiple times.
*   **Explain Analyze:** Forces the target database to generate a real execution plan and metrics for each query, saving the Plan JSON for visualization.

## Advanced Usage

### GUC Parameter Tuning

| Parameter | Default | Context | Description |
| :--- | :--- | :--- | :--- |
| `pg_rat.enabled` | `on` | SUSET | Global switch to enable/disable capture logic. |
| `pg_rat.ring_buffer_size` | `8192` | POSTMASTER | Number of event slots in shared memory. Increase for high-TPS workloads. |
| `pg_rat.max_query_length` | `4096` | POSTMASTER | Truncates long SQL strings to save memory/disk. |
| `pg_rat.capture_directory` | `pg_rat_capture` | SIGHUP | Base directory under `PGDATA` for output. |

### Export & Import
You can move captures between servers using the built-in archive functions:
```sql
-- Creates an archive of 'my_capture' on the server filesystem
SELECT pg_rat_export_capture('/tmp/workload_v1.tar.gz');

-- On target server, import it
SELECT pg_rat_import_capture('/tmp/workload_v1.tar.gz');
```

## Internal Data Format
Workload files (`.ndjson`) use the following schema:
```json
{
  "ts": 1714286400000000, 
  "sid": "0000abcd", 
  "xid": 1234, 
  "type": "QUERY_END", 
  "dur_ms": 1.25, 
  "rows": 10, 
  "shblk_hit": 5, 
  "shblk_read": 0, 
  "query": "SELECT * FROM users WHERE id = 1"
}
```
*   `ts`: Microseconds since Unix Epoch.
*   `sid`: Session ID (hashed Pid + StartTime).
*   `xid`: Local Virtual Transaction ID.
