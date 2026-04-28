/* contrib/pg_rat/pg_rat--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_rat" to load this file. \quit

-- ========================================================================
-- SPA Results Table
-- ========================================================================

CREATE TABLE IF NOT EXISTS pg_rat_spa_results (
    query_hash      bigint,
    query_text      text,
    source_elapsed_ms   double precision,
    target_elapsed_ms   double precision,
    source_buffers  bigint,
    target_buffers  bigint,
    regression_pct  double precision,
    plan_changed    boolean,
    target_plan_json    text,
    exec_count      integer
);

-- ========================================================================
-- Capture Control Functions
-- ========================================================================

CREATE FUNCTION pg_rat_start_capture(capture_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rat_start_capture'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_rat_start_capture(text) IS
    'Start capturing workload into the named capture session';

CREATE FUNCTION pg_rat_stop_capture()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rat_stop_capture'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_rat_stop_capture() IS
    'Stop the active workload capture session';

CREATE FUNCTION pg_rat_capture_status(
    OUT active boolean,
    OUT capture_name text,
    OUT started_at timestamptz,
    OUT total_events bigint,
    OUT dropped_events bigint,
    OUT ring_buffer_size integer
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_rat_capture_status'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_rat_capture_status() IS
    'Return the current capture session status';

CREATE FUNCTION pg_rat_export_capture(filepath text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rat_export_capture'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_rat_export_capture(text) IS
    'Export the current capture to a tar.gz archive';

CREATE FUNCTION pg_rat_import_capture(filepath text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rat_import_capture'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_rat_import_capture(text) IS
    'Import a capture archive into the capture directory';

-- ========================================================================
-- Replay Functions
-- ========================================================================

CREATE FUNCTION pg_rat_start_replay(
    capture_dir text,
    synchronization boolean DEFAULT true
)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rat_start_replay'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_rat_start_replay(text, boolean) IS
    'Start replaying a captured workload with original concurrency and timing';

CREATE FUNCTION pg_rat_replay_status(
    OUT active boolean,
    OUT sessions integer,
    OUT events_done bigint,
    OUT events_total bigint,
    OUT errors bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_rat_replay_status'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_rat_replay_status() IS
    'Return the current replay session progress';

-- ========================================================================
-- SQL Performance Analyzer (SPA) Functions
-- ========================================================================

CREATE FUNCTION pg_rat_run_spa(
    capture_dir text,
    comparison_metric text DEFAULT 'elapsed_time'
)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rat_run_spa'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pg_rat_run_spa(text, text) IS
    'Run SQL Performance Analysis on the captured workload';

-- ========================================================================
-- Report Generator
-- ========================================================================

CREATE FUNCTION pg_rat_generate_report(filepath text DEFAULT NULL)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_rat_generate_report'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_rat_generate_report(text) IS
    'Generate an HTML regression report from SPA results';
