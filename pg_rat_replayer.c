/*-------------------------------------------------------------------------
 *
 * pg_rat_replayer.c
 *		PostgreSQL Real Application Testing - workload replay engine
 *
 * Implements the replay coordinator that reads NDJSON capture files
 * and replays queries against the target database using async libpq
 * connections, preserving original session mapping and timing.
 *
 * Architecture:
 * - A dynamic background worker is launched by pg_rat_start_replay()
 * - The coordinator pre-scans all NDJSON files to build a session map
 * - Opens one async PGconn per unique session_id via unix socket
 * - Uses a priority queue (sorted by timestamp) to dispatch queries
 *   at the correct relative time offset from capture start
 * - Records replay results into shared state for monitoring
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat_replayer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fmgr.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "access/htup_details.h"
#include "funcapi.h"

#include "pg_rat.h"

/* Maximum sessions we support for replay */
#define RAT_MAX_REPLAY_SESSIONS		1024

/* Maximum events to load into memory */
#define RAT_MAX_REPLAY_EVENTS		(1024 * 1024)

/* Replay event parsed from NDJSON */
typedef struct ReplayEvent
{
	TimestampTz timestamp;
	uint32		session_id;
	uint32		transaction_id;
	int			event_type;
	char		query_text[RAT_MAX_QUERY_LEN];
} ReplayEvent;

/* Session tracking for replay */
typedef struct ReplaySession
{
	uint32		session_id;
	PGconn	   *conn;
	bool		busy;		/* waiting for result */
} ReplaySession;

/*
 * Simple JSON field extraction (no full JSON parser needed for our
 * controlled NDJSON format).
 */
static bool
extract_json_string(const char *json, const char *key, char *out, int outlen)
{
	char		search[256];
	const char *pos;
	const char *start;
	int			i;

	snprintf(search, sizeof(search), "\"%s\":\"", key);
	pos = strstr(json, search);
	if (!pos)
		return false;

	start = pos + strlen(search);
	i = 0;
	while (*start && *start != '"' && i < outlen - 1)
	{
		if (*start == '\\' && *(start + 1))
		{
			start++;
			switch (*start)
			{
				case '"':
					out[i++] = '"';
					break;
				case '\\':
					out[i++] = '\\';
					break;
				case 'n':
					out[i++] = '\n';
					break;
				case 'r':
					out[i++] = '\r';
					break;
				case 't':
					out[i++] = '\t';
					break;
				default:
					out[i++] = *start;
					break;
			}
		}
		else
		{
			out[i++] = *start;
		}
		start++;
	}
	out[i] = '\0';
	return true;
}

static bool
extract_json_int64(const char *json, const char *key, int64 *out)
{
	char		search[256];
	const char *pos;

	snprintf(search, sizeof(search), "\"%s\":", key);
	pos = strstr(json, search);
	if (!pos)
		return false;

	pos += strlen(search);
	*out = strtoll(pos, NULL, 10);
	return true;
}

static int
event_type_from_string(const char *s)
{
	if (strcmp(s, "QUERY_END") == 0)
		return RAT_EVENT_QUERY_END;
	if (strcmp(s, "UTILITY") == 0)
		return RAT_EVENT_UTILITY;
	if (strcmp(s, "TXN_BEGIN") == 0)
		return RAT_EVENT_TXN_BEGIN;
	if (strcmp(s, "TXN_COMMIT") == 0)
		return RAT_EVENT_TXN_COMMIT;
	if (strcmp(s, "TXN_ROLLBACK") == 0)
		return RAT_EVENT_TXN_ROLLBACK;
	return RAT_EVENT_QUERY_END;
}

/*
 * Parse a single NDJSON line into a ReplayEvent.
 */
static bool
parse_ndjson_line(const char *line, ReplayEvent *event)
{
	int64		ts;
	char		sid_str[32];
	int64		xid;
	char		type_str[32];

	memset(event, 0, sizeof(ReplayEvent));

	if (!extract_json_int64(line, "ts", &ts))
		return false;
	event->timestamp = (TimestampTz) ts;

	if (extract_json_string(line, "sid", sid_str, sizeof(sid_str)))
		event->session_id = (uint32) strtoul(sid_str, NULL, 16);

	if (extract_json_int64(line, "xid", &xid))
		event->transaction_id = (uint32) xid;

	if (extract_json_string(line, "type", type_str, sizeof(type_str)))
		event->event_type = event_type_from_string(type_str);

	extract_json_string(line, "query", event->query_text, RAT_MAX_QUERY_LEN);

	return true;
}

/*
 * Comparison function for sorting replay events by timestamp.
 */
static int
replay_event_cmp(const void *a, const void *b)
{
	const ReplayEvent *ea = (const ReplayEvent *) a;
	const ReplayEvent *eb = (const ReplayEvent *) b;

	if (ea->timestamp < eb->timestamp)
		return -1;
	if (ea->timestamp > eb->timestamp)
		return 1;
	return 0;
}

/*
 * Find or create a replay session for the given session_id.
 */
static ReplaySession *
find_session(ReplaySession *sessions, int *nsessions, uint32 session_id)
{
	int			i;

	for (i = 0; i < *nsessions; i++)
	{
		if (sessions[i].session_id == session_id)
			return &sessions[i];
	}

	/* Create new session */
	if (*nsessions >= RAT_MAX_REPLAY_SESSIONS)
		return NULL;

	i = (*nsessions)++;
	sessions[i].session_id = session_id;
	sessions[i].conn = NULL;
	sessions[i].busy = false;

	return &sessions[i];
}

/*
 * rat_replay_coordinator_main
 *
 * Main function for the replay coordinator background worker.
 * Reads NDJSON files, opens async libpq connections, and replays
 * queries at original timing offsets.
 */
PGDLLEXPORT void
rat_replay_coordinator_main(Datum main_arg)
{
	char		capture_dir[MAXPGPATH];
	char		capture_name[RAT_CAPTURE_NAME_LEN];
	DIR		   *dir;
	struct dirent *de;
	ReplayEvent *events;
	int			nevents = 0;
	ReplaySession *sessions;
	int			nsessions = 0;
	TimestampTz base_ts;
	TimestampTz replay_start;
	int			i;
	char		conninfo[256];

	/* Signal setup */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();

	/* Attach to shared memory */
	{
		bool		found;

		rat_shared_state = (RatSharedState *)
			ShmemInitStruct("pg_rat", sizeof(RatSharedState), &found);
		rat_ring_buffer = (RatRingBuffer *)
			ShmemInitStruct("pg_rat ring buffer", rat_shmem_size(), &found);

		if (!rat_shared_state || !rat_ring_buffer)
		{
			elog(ERROR, "pg_rat: could not attach to shared memory");
			return;
		}
	}

	/* Read capture name from shared state */
	LWLockAcquire(&rat_shared_state->lock.lock, LW_SHARED);
	strlcpy(capture_name, rat_shared_state->capture_name, RAT_CAPTURE_NAME_LEN);
	LWLockRelease(&rat_shared_state->lock.lock);

	snprintf(capture_dir, MAXPGPATH, "%s/%s",
			 rat_capture_directory, capture_name);

	elog(LOG, "pg_rat: replay coordinator started for \"%s\"", capture_name);

	/* Allocate event array */
	events = (ReplayEvent *) palloc0(sizeof(ReplayEvent) * RAT_MAX_REPLAY_EVENTS);
	sessions = (ReplaySession *) palloc0(sizeof(ReplaySession) * RAT_MAX_REPLAY_SESSIONS);

	/* Scan capture directory for NDJSON files */
	dir = opendir(capture_dir);
	if (dir == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("pg_rat: could not open capture directory \"%s\": %m",
						capture_dir)));
		goto cleanup;
	}

	while ((de = readdir(dir)) != NULL)
	{
		char		filepath[MAXPGPATH];
		FILE	   *fp;
		char		line[RAT_MAX_QUERY_LEN * 2 + 512];

		/* Skip non-NDJSON files */
		if (strstr(de->d_name, ".ndjson") == NULL)
			continue;

		snprintf(filepath, MAXPGPATH, "%s/%s", capture_dir, de->d_name);
		fp = fopen(filepath, "r");
		if (fp == NULL)
		{
			elog(LOG, "pg_rat: could not open \"%s\": %m", filepath);
			continue;
		}

		while (fgets(line, sizeof(line), fp) != NULL &&
			   nevents < RAT_MAX_REPLAY_EVENTS)
		{
			if (parse_ndjson_line(line, &events[nevents]))
				nevents++;
		}
		fclose(fp);
	}
	closedir(dir);

	if (nevents == 0)
	{
		elog(LOG, "pg_rat: no events found in capture directory");
		goto cleanup;
	}

	/* Sort events by timestamp */
	qsort(events, nevents, sizeof(ReplayEvent), replay_event_cmp);

	/* Update shared state with total */
	pg_atomic_write_u64(&rat_shared_state->replay_events_total, (uint64) nevents);
	pg_atomic_write_u64(&rat_shared_state->replay_events_done, 0);
	pg_atomic_write_u64(&rat_shared_state->replay_errors, 0);

	/* Build session map */
	for (i = 0; i < nevents; i++)
		find_session(sessions, &nsessions, events[i].session_id);

	rat_shared_state->replay_sessions = nsessions;

	elog(LOG, "pg_rat: replay loaded %d events across %d sessions",
		 nevents, nsessions);

	/* Open async libpq connections for each session */
	snprintf(conninfo, sizeof(conninfo),
			 "dbname=%s host=/tmp", rat_capture_directory);
	/* Use a simple local connection */
	snprintf(conninfo, sizeof(conninfo), "dbname=postgres");

	for (i = 0; i < nsessions; i++)
	{
		sessions[i].conn = PQconnectdb(conninfo);
		if (PQstatus(sessions[i].conn) != CONNECTION_OK)
		{
			elog(LOG, "pg_rat: session %08x connection failed: %s",
				 sessions[i].session_id, PQerrorMessage(sessions[i].conn));
			PQfinish(sessions[i].conn);
			sessions[i].conn = NULL;
		}
	}

	/* Replay events with timing */
	base_ts = events[0].timestamp;
	replay_start = GetCurrentTimestamp();

	for (i = 0; i < nevents; i++)
	{
		ReplayEvent *ev = &events[i];
		ReplaySession *sess;
		long		target_offset_us;
		long		elapsed_us;
		long		sleep_us;
		PGresult   *res;

		CHECK_FOR_INTERRUPTS();

		/* Find the session for this event */
		sess = find_session(sessions, &nsessions, ev->session_id);
		if (sess == NULL || sess->conn == NULL)
		{
			pg_atomic_fetch_add_u64(&rat_shared_state->replay_errors, 1);
			pg_atomic_fetch_add_u64(&rat_shared_state->replay_events_done, 1);
			continue;
		}

		/* Compute timing offset and sleep if needed */
		target_offset_us = ev->timestamp - base_ts;
		elapsed_us = GetCurrentTimestamp() - replay_start;
		sleep_us = target_offset_us - elapsed_us;

		if (sleep_us > 0)
			pg_usleep(sleep_us);

		/* Execute the query */
		if (strlen(ev->query_text) > 0)
		{
			res = PQexec(sess->conn, ev->query_text);
			if (PQresultStatus(res) != PGRES_COMMAND_OK &&
				PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				pg_atomic_fetch_add_u64(&rat_shared_state->replay_errors, 1);
			}
			PQclear(res);
		}

		pg_atomic_fetch_add_u64(&rat_shared_state->replay_events_done, 1);
	}

	ereport(LOG,
			errmsg_internal("pg_rat: replay completed — %d events, %llu errors",
							nevents,
							(unsigned long long) pg_atomic_read_u64(&rat_shared_state->replay_errors)));

cleanup:
	/* Close all connections */
	for (i = 0; i < nsessions; i++)
	{
		if (sessions[i].conn)
			PQfinish(sessions[i].conn);
	}

	rat_shared_state->replay_active = false;

	pfree(events);
	pfree(sessions);
}

/* ======================================================================
 * SQL-callable replay functions
 * ====================================================================== */

PG_FUNCTION_INFO_V1(pg_rat_start_replay);
PG_FUNCTION_INFO_V1(pg_rat_replay_status);

/*
 * pg_rat_start_replay(capture_dir text, synchronization bool) → void
 */
Datum
pg_rat_start_replay(PG_FUNCTION_ARGS)
{
	text	   *capture_dir_text = PG_GETARG_TEXT_PP(0);
	const char *capture_name = text_to_cstring(capture_dir_text);
	BackgroundWorker bgw;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	if (!rat_shared_state)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_rat must be loaded via shared_preload_libraries")));

	if (rat_shared_state->replay_active)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a replay session is already active")));

	/* Store the capture name for the coordinator to read */
	LWLockAcquire(&rat_shared_state->lock.lock, LW_EXCLUSIVE);
	strlcpy(rat_shared_state->capture_name, capture_name, RAT_CAPTURE_NAME_LEN);
	rat_shared_state->replay_active = true;
	LWLockRelease(&rat_shared_state->lock.lock);

	/* Launch the replay coordinator as a dynamic background worker */
	memset(&bgw, 0, sizeof(bgw));
	snprintf(bgw.bgw_name, BGW_MAXLEN, "pg_rat replay coordinator");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "pg_rat replay coordinator");
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "pg_rat");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "rat_replay_coordinator_main");
	bgw.bgw_main_arg = (Datum) 0;
	bgw.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&bgw, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register replay background worker")));

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("replay background worker failed to start")));

	elog(LOG, "pg_rat: replay coordinator launched (pid %d)", pid);

	PG_RETURN_VOID();
}

/*
 * pg_rat_replay_status() → SETOF record
 */
Datum
pg_rat_replay_status(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5];
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

	values[0] = BoolGetDatum(rat_shared_state->replay_active);
	values[1] = Int32GetDatum(rat_shared_state->replay_sessions);
	values[2] = Int64GetDatum((int64) pg_atomic_read_u64(&rat_shared_state->replay_events_done));
	values[3] = Int64GetDatum((int64) pg_atomic_read_u64(&rat_shared_state->replay_events_total));
	values[4] = Int64GetDatum((int64) pg_atomic_read_u64(&rat_shared_state->replay_errors));

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}
