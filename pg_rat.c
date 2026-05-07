/*-------------------------------------------------------------------------
 *
 * pg_rat.c
 *		PostgreSQL Real Application Testing - module entry point
 *
 * This is the main module for the pg_rat extension. It:
 *   - Validates shared_preload_libraries loading
 *   - Defines GUC variables
 *   - Registers shared memory callbacks
 *   - Installs ExecutorEnd and ProcessUtility hooks
 *   - Registers the background worker for asynchronous NDJSON flushing
 *   - Implements SQL-callable control functions (start/stop capture, etc.)
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#include "pg_rat.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_rat",
					.version = PG_VERSION
);

/* ----------------
 * GUC variables
 * ----------------
 */
bool		rat_enabled = true;
int			rat_ring_buffer_size = RAT_DEFAULT_RING_SIZE;
int			rat_max_query_length = RAT_MAX_QUERY_LEN;
char	   *rat_capture_directory = NULL;

/* ----------------
 * Shared memory pointers
 * ----------------
 */
RatSharedState *rat_shared_state = NULL;
RatRingBuffer *rat_ring_buffer = NULL;


/* ----------------
 * Saved hook values
 * ----------------
 */
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ----------------
 * Shmem callbacks
 * ----------------
 */
static const ShmemCallbacks rat_shmem_callbacks = {
	.request_fn = rat_shmem_request,
	.init_fn = rat_shmem_init,
};

/* ----------------
 * Local hook wrappers (chain to capture functions and then to prev hooks)
 * ----------------
 */
static void
rat_ExecutorEnd_hook(QueryDesc *queryDesc)
{
	if (rat_enabled && rat_shared_state &&
		pg_atomic_read_u32(&rat_shared_state->capture_active))
	{
		PG_TRY();
		{
			rat_ExecutorEnd(queryDesc);
		}
		PG_CATCH();
		{
			/* Never let capture errors crash the query */
			FlushErrorState();
		}
		PG_END_TRY();
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

static void
rat_ProcessUtility_hook(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	if (rat_enabled && rat_shared_state &&
		pg_atomic_read_u32(&rat_shared_state->capture_active))
		rat_ProcessUtility(pstmt, queryString, readOnlyTree, context,
						   params, queryEnv, dest, qc);

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* ----------------
 * Module load callback
 * ----------------
 */
void
_PG_init(void)
{
	BackgroundWorker bgw;

	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Define GUC variables
	 */
	DefineCustomBoolVariable("pg_rat.enabled",
							 "Enable pg_rat workload capture.",
							 NULL,
							 &rat_enabled,
							 true,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_rat.ring_buffer_size",
							"Number of slots in the shared memory ring buffer.",
							NULL,
							&rat_ring_buffer_size,
							RAT_DEFAULT_RING_SIZE,
							256,
							RAT_MAX_RING_SIZE,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomIntVariable("pg_rat.max_query_length",
							"Maximum query text length captured per event.",
							NULL,
							&rat_max_query_length,
							RAT_MAX_QUERY_LEN,
							256,
							32768,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);

	DefineCustomStringVariable("pg_rat.capture_directory",
							   "Directory for capture output files (relative to PGDATA).",
							   NULL,
							   &rat_capture_directory,
							   RAT_CAPTURE_DIR,
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_rat");

	/*
	 * Register shared memory callbacks
	 */
	RegisterShmemCallbacks(&rat_shmem_callbacks);

	/*
	 * Install executor and utility hooks
	 */
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = rat_ExecutorEnd_hook;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = rat_ProcessUtility_hook;

	/*
	 * Register the flusher background worker
	 */
	memset(&bgw, 0, sizeof(bgw));
	snprintf(bgw.bgw_name, BGW_MAXLEN, "pg_rat flusher");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "pg_rat flusher");
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "pg_rat");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "rat_bgworker_main");
	bgw.bgw_main_arg = (Datum) 0;
	bgw.bgw_notify_pid = 0;
	RegisterBackgroundWorker(&bgw);
}

/* ======================================================================
 * SQL-callable functions
 * ====================================================================== */

PG_FUNCTION_INFO_V1(pg_rat_start_capture);
PG_FUNCTION_INFO_V1(pg_rat_stop_capture);
PG_FUNCTION_INFO_V1(pg_rat_capture_status);
PG_FUNCTION_INFO_V1(pg_rat_export_capture);
PG_FUNCTION_INFO_V1(pg_rat_import_capture);

/*
 * pg_rat_start_capture(capture_name text) → void
 *
 * Begins workload capture. Creates the output directory under PGDATA
 * and sets the shared state to active.
 */
Datum
pg_rat_start_capture(PG_FUNCTION_ARGS)
{
	text	   *capture_name_text;
	const char *capture_name;
	char		dirpath[MAXPGPATH];
	struct stat st;

	if (!rat_shared_state)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_rat must be loaded via shared_preload_libraries")));

	if (pg_atomic_read_u32(&rat_shared_state->capture_active))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a capture session is already active: \"%s\"",
						rat_shared_state->capture_name)));

	capture_name_text = PG_GETARG_TEXT_PP(0);
	capture_name = text_to_cstring(capture_name_text);

	if (strlen(capture_name) == 0 || strlen(capture_name) >= RAT_CAPTURE_NAME_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("capture name must be between 1 and %d characters",
						RAT_CAPTURE_NAME_LEN - 1)));

	/* Create capture directory: $PGDATA/<capture_dir>/<capture_name>/ */
	snprintf(dirpath, MAXPGPATH, "%s/%s",
			 rat_capture_directory, capture_name);

	if (stat(dirpath, &st) != 0)
	{
		/* Create parent directory if needed */
		if (pg_mkdir_p(rat_capture_directory, S_IRWXU) != 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							rat_capture_directory)));

		if (pg_mkdir_p(dirpath, S_IRWXU) != 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							dirpath)));
	}

	/* Activate capture in shared state */
	LWLockAcquire(&rat_shared_state->lock.lock, LW_EXCLUSIVE);
	strlcpy(rat_shared_state->capture_name, capture_name, RAT_CAPTURE_NAME_LEN);
	rat_shared_state->capture_start = GetCurrentTimestamp();
	pg_atomic_write_u64(&rat_shared_state->total_events, 0);
	pg_atomic_write_u64(&rat_ring_buffer->dropped_events, 0);
	pg_atomic_write_u32(&rat_shared_state->capture_active, 1);
	LWLockRelease(&rat_shared_state->lock.lock);

	/* Wake the flusher BGW */
	if (rat_shared_state->bgw_latch)
		SetLatch(rat_shared_state->bgw_latch);

	elog(LOG, "pg_rat: capture started: \"%s\"", capture_name);

	PG_RETURN_VOID();
}

/*
 * pg_rat_stop_capture() → void
 */
Datum
pg_rat_stop_capture(PG_FUNCTION_ARGS)
{
	if (!rat_shared_state)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_rat must be loaded via shared_preload_libraries")));

	if (!pg_atomic_read_u32(&rat_shared_state->capture_active))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no capture session is currently active")));

	LWLockAcquire(&rat_shared_state->lock.lock, LW_EXCLUSIVE);
	pg_atomic_write_u32(&rat_shared_state->capture_active, 0);
	LWLockRelease(&rat_shared_state->lock.lock);

	/* Wake flusher to drain remaining events */
	if (rat_shared_state->bgw_latch)
		SetLatch(rat_shared_state->bgw_latch);

	{
		uint64		total = pg_atomic_read_u64(&rat_shared_state->total_events);
		uint64		dropped = pg_atomic_read_u64(&rat_ring_buffer->dropped_events);

		ereport(LOG,
				errmsg_internal("pg_rat: capture stopped: \"%s\" (events captured: %llu, dropped: %llu)",
								rat_shared_state->capture_name,
								(unsigned long long) total,
								(unsigned long long) dropped));
	}

	PG_RETURN_VOID();
}

/*
 * pg_rat_capture_status() → SETOF record
 *
 * Returns a single row describing the current capture state.
 */
Datum
pg_rat_capture_status(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6];
	HeapTuple	htup;

	if (!rat_shared_state)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_rat must be loaded via shared_preload_libraries")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context that cannot accept type record")));

	memset(nulls, 0, sizeof(nulls));

	values[0] = BoolGetDatum(pg_atomic_read_u32(&rat_shared_state->capture_active) != 0);
	values[1] = CStringGetTextDatum(rat_shared_state->capture_name);
	if (pg_atomic_read_u32(&rat_shared_state->capture_active))
		values[2] = TimestampTzGetDatum(rat_shared_state->capture_start);
	else
		nulls[2] = true;
	values[3] = Int64GetDatum((int64) pg_atomic_read_u64(&rat_shared_state->total_events));
	values[4] = Int64GetDatum((int64) pg_atomic_read_u64(&rat_ring_buffer->dropped_events));
	values[5] = Int32GetDatum(rat_ring_buffer_size);

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * pg_rat_export_capture(filepath text) → void
 *
 * Creates a tar.gz archive of the capture directory. Currently a stub
 * that copies the NDJSON directory path to the user.
 */
/*
 * Validate a path for safe use in shell commands.
 * Rejects characters that could cause shell injection.
 */
static void
rat_validate_shell_path(const char *path)
{
	const char *p;

	for (p = path; *p; p++)
	{
		if (*p == ';' || *p == '|' || *p == '&' || *p == '$' ||
			*p == '`' || *p == '\'' || *p == '"' || *p == '(' ||
			*p == ')' || *p == '<' || *p == '>' || *p == '\n' ||
			*p == '\r' || *p == ' ')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("path contains unsafe character '%c'", *p)));
	}
}

Datum
pg_rat_export_capture(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	const char *filepath = text_to_cstring(filepath_text);
	char		cmd[MAXPGPATH * 2 + 64];
	int			rc;

	if (!rat_shared_state)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_rat must be loaded via shared_preload_libraries")));

	if (strlen(rat_shared_state->capture_name) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("no capture session has been recorded")));

	/* Validate paths to prevent shell injection */
	rat_validate_shell_path(filepath);
	rat_validate_shell_path(rat_capture_directory);
	rat_validate_shell_path(rat_shared_state->capture_name);

	snprintf(cmd, sizeof(cmd), "tar czf %s -C %s %s",
			 filepath, rat_capture_directory, rat_shared_state->capture_name);

	rc = system(cmd);
	if (rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("could not create archive: command returned %d", rc)));

	elog(LOG, "pg_rat: exported capture to \"%s\"", filepath);

	PG_RETURN_VOID();
}

/*
 * pg_rat_import_capture(filepath text) → void
 *
 * Extracts a tar.gz archive into the capture directory.
 */
Datum
pg_rat_import_capture(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	const char *filepath = text_to_cstring(filepath_text);
	char		cmd[MAXPGPATH * 2 + 64];
	int			rc;

	/* Validate paths to prevent shell injection */
	rat_validate_shell_path(filepath);
	rat_validate_shell_path(rat_capture_directory);

	/* Create capture directory if needed */
	if (MakePGDirectory(rat_capture_directory) != 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m",
						rat_capture_directory)));

	snprintf(cmd, sizeof(cmd), "tar xzf %s -C %s",
			 filepath, rat_capture_directory);

	rc = system(cmd);
	if (rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("could not extract archive: command returned %d", rc)));

	elog(LOG, "pg_rat: imported capture from \"%s\"", filepath);

	PG_RETURN_VOID();
}
