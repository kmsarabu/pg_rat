/*-------------------------------------------------------------------------
 *
 * pg_rat_bgworker.c
 *		PostgreSQL Real Application Testing - background worker (flusher)
 *
 * Implements the background worker that drains the shared memory ring
 * buffer and writes captured events to NDJSON files on disk.
 *
 * The flusher wakes periodically (default 100ms) or on latch set,
 * drains all available events from the ring buffer, and writes them
 * as newline-delimited JSON to files under the capture directory.
 *
 * Files are rotated at a configurable size (default 64MB).
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat_bgworker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "utils/guc.h"

#include "pg_rat.h"

/* Drain batch size */
#define RAT_DRAIN_BATCH		1024

/* Event type names for NDJSON output */
static const char *
rat_event_type_name(int event_type)
{
	switch (event_type)
	{
		case RAT_EVENT_QUERY_END:
			return "QUERY_END";
		case RAT_EVENT_UTILITY:
			return "UTILITY";
		case RAT_EVENT_TXN_BEGIN:
			return "TXN_BEGIN";
		case RAT_EVENT_TXN_COMMIT:
			return "TXN_COMMIT";
		case RAT_EVENT_TXN_ROLLBACK:
			return "TXN_ROLLBACK";
		default:
			return "UNKNOWN";
	}
}

/*
 * Escape a string for JSON output.
 * Writes the escaped string into dst (which must be at least 2*srclen+1 bytes).
 * Returns the number of bytes written (not including the null terminator).
 */
static int
rat_json_escape(char *dst, const char *src, int srclen)
{
	int			i,
				j = 0;

	for (i = 0; i < srclen && src[i] != '\0'; i++)
	{
		unsigned char c = (unsigned char) src[i];

		switch (c)
		{
			case '"':
				dst[j++] = '\\';
				dst[j++] = '"';
				break;
			case '\\':
				dst[j++] = '\\';
				dst[j++] = '\\';
				break;
			case '\b':
				dst[j++] = '\\';
				dst[j++] = 'b';
				break;
			case '\f':
				dst[j++] = '\\';
				dst[j++] = 'f';
				break;
			case '\n':
				dst[j++] = '\\';
				dst[j++] = 'n';
				break;
			case '\r':
				dst[j++] = '\\';
				dst[j++] = 'r';
				break;
			case '\t':
				dst[j++] = '\\';
				dst[j++] = 't';
				break;
			default:
				if (c < 0x20)
				{
					/* Control character: encode as \u00XX */
					j += snprintf(dst + j, 7, "\\u%04x", c);
				}
				else
				{
					dst[j++] = c;
				}
				break;
		}
	}

	dst[j] = '\0';
	return j;
}

/*
 * rat_bgworker_main
 *
 * Main entry point for the flusher background worker.
 */
PGDLLEXPORT void
rat_bgworker_main(Datum main_arg)
{
	RatEvent   *drain_buf;
	FILE	   *outfile = NULL;
	char		filepath[MAXPGPATH];
	int			file_seq = 0;
	long		file_bytes = 0;
	char	   *escaped_buf;
	char	   *line_buf;
	char		dirpath[MAXPGPATH];

	/* Register signal handlers */
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


	/* Register our latch so capture code can wake us */
	rat_shared_state->bgw_latch = MyLatch;

	elog(LOG, "pg_rat: flusher background worker started");

	/* Allocate drain buffer in local memory (not shared) */
	drain_buf = (RatEvent *) palloc(sizeof(RatEvent) * RAT_DRAIN_BATCH);

	/* Buffer for JSON-escaped query text: worst case 2x + some slack */
	escaped_buf = (char *) palloc(RAT_MAX_QUERY_LEN * 2 + 16);

	/* Line buffer for NDJSON output */
	line_buf = (char *) palloc(RAT_MAX_QUERY_LEN * 2 + 1024);

	/* Main loop */
	for (;;)
	{
		int			count;

		/* Check for shutdown */
		CHECK_FOR_INTERRUPTS();

		/* Wait for latch or timeout */
		(void) WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   RAT_BGW_NAPTIME_MS,
					   PG_WAIT_EXTENSION);

		ResetLatch(MyLatch);

		/* Reload config if signaled */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (ShutdownRequestPending)
			break;

		/* If capture is not active, close any open file and sleep */
		if (!pg_atomic_read_u32(&rat_shared_state->capture_active))
		{
			if (outfile != NULL)
			{
				/* Drain remaining events before closing */
				count = rat_ring_buffer_drain(drain_buf, RAT_DRAIN_BATCH);
				if (count > 0 && outfile != NULL)
				{
					int			i;

					for (i = 0; i < count; i++)
					{
						RatEvent   *ev = &drain_buf[i];
						int			line_len;

						(void) rat_json_escape(escaped_buf,
													  ev->query_text,
													  ev->query_len);

						line_len = snprintf(line_buf,
											RAT_MAX_QUERY_LEN * 2 + 1024,
											"{\"ts\":%lld,"
											"\"sid\":\"%08x\","
											"\"xid\":%u,"
											"\"type\":\"%s\","
											"\"dur_ms\":%.3f,"
											"\"rows\":%lld,"
											"\"shblk_hit\":%lld,"
											"\"shblk_read\":%lld,"
											"\"qlen\":%d,"
											"\"query\":\"%s\"}\n",
											(long long) ev->timestamp,
											ev->session_id,
											ev->transaction_id,
											rat_event_type_name(ev->event_type),
											ev->duration_ms,
											(long long) ev->rows,
											(long long) ev->shared_blks_hit,
											(long long) ev->shared_blks_read,
											ev->query_len,
											escaped_buf);

						fwrite(line_buf, 1, line_len, outfile);
						file_bytes += line_len;
					}
				}
				fflush(outfile);
				fclose(outfile);
				outfile = NULL;
				file_seq = 0;
				file_bytes = 0;
			}
			continue;
		}

		/* Drain available events */
		count = rat_ring_buffer_drain(drain_buf, RAT_DRAIN_BATCH);

		if (count == 0)
			continue;

		/* Open output file if needed, or rotate */
		if (outfile == NULL || file_bytes >= RAT_FILE_ROTATE_SIZE)
		{
			if (outfile != NULL)
			{
				fflush(outfile);
				fclose(outfile);
			}

			snprintf(dirpath, MAXPGPATH, "%s/%s",
					 rat_capture_directory,
					 rat_shared_state->capture_name);

			snprintf(filepath, MAXPGPATH, "%s/capture_%04d.ndjson",
					 dirpath,
					 file_seq++);

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

			outfile = fopen(filepath, "a");
			if (outfile == NULL)
			{
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("pg_rat: could not open file \"%s\": %m",
								filepath)));
				continue;
			}
			file_bytes = 0;
		}

		/* Write events as NDJSON */
		{
			int			i;

			for (i = 0; i < count; i++)
			{
				RatEvent   *ev = &drain_buf[i];
				int			line_len;

				(void) rat_json_escape(escaped_buf,
											  ev->query_text,
											  ev->query_len);

				line_len = snprintf(line_buf,
									RAT_MAX_QUERY_LEN * 2 + 1024,
									"{\"ts\":%lld,"
									"\"sid\":\"%08x\","
									"\"xid\":%u,"
									"\"type\":\"%s\","
									"\"dur_ms\":%.3f,"
									"\"rows\":%lld,"
									"\"shblk_hit\":%lld,"
									"\"shblk_read\":%lld,"
									"\"qlen\":%d,"
									"\"query\":\"%s\"}\n",
									(long long) ev->timestamp,
									ev->session_id,
									ev->transaction_id,
									rat_event_type_name(ev->event_type),
									ev->duration_ms,
									(long long) ev->rows,
									(long long) ev->shared_blks_hit,
									(long long) ev->shared_blks_read,
									ev->query_len,
									escaped_buf);

				fwrite(line_buf, 1, line_len, outfile);
				file_bytes += line_len;
			}

			fflush(outfile);
		}
	}

	/* Cleanup (unreachable in normal operation) */
	if (outfile)
		fclose(outfile);
	pfree(drain_buf);
	pfree(escaped_buf);
	pfree(line_buf);
}
