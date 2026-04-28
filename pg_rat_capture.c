/*-------------------------------------------------------------------------
 *
 * pg_rat_capture.c
 *		PostgreSQL Real Application Testing - hook implementations
 *
 * Implements the ExecutorEnd and ProcessUtility hook callbacks that
 * capture query events into the shared memory ring buffer.
 *
 * Design rationale:
 * - ExecutorEnd is used (not ExecutorStart/Run) because at this point
 *   the query is fully executed and we have accurate timing, row counts,
 *   and buffer usage. This matches the pg_stat_statements pattern.
 * - ProcessUtility captures DDL, transaction control (BEGIN/COMMIT/ROLLBACK),
 *   SET, COPY, etc. -- essential for faithful replay.
 * - The hot path does zero palloc, no disk I/O, no LWLock. We only
 *   do a SpinLock-protected memcpy of a fixed-size struct into the
 *   ring buffer.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat_capture.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/timestamp.h"

#include "pg_rat.h"

/*
 * rat_ExecutorEnd
 *
 * Called from the ExecutorEnd hook when capture is active.
 * Builds a RatEvent from the QueryDesc and pushes it to the ring buffer.
 */
void
rat_ExecutorEnd(QueryDesc *queryDesc)
{
	RatEvent	event;
	const char *query_text;
	int			qlen;

	/* Safety: skip if no query text available */
	if (queryDesc->sourceText == NULL)
		return;

	memset(&event, 0, sizeof(RatEvent));

	event.timestamp = GetCurrentTimestamp();
	event.session_id = rat_compute_session_id();

	/* Use the current virtual transaction ID as our transaction_id */
	{
		LocalTransactionId lxid = MyProc ? MyProc->vxid.lxid : InvalidLocalTransactionId;

		event.transaction_id = (uint32) lxid;
	}

	event.event_type = RAT_EVENT_QUERY_END;

	/* Timing: if instrumentation is available, use it */
	if (queryDesc->query_instr)
	{
		event.duration_ms = INSTR_TIME_GET_MILLISEC(queryDesc->query_instr->total);
		event.shared_blks_hit = queryDesc->query_instr->bufusage.shared_blks_hit;
		event.shared_blks_read = queryDesc->query_instr->bufusage.shared_blks_read;
	}
	else
	{
		event.duration_ms = 0.0;
		event.shared_blks_hit = 0;
		event.shared_blks_read = 0;
	}

	event.rows = queryDesc->estate->es_total_processed;

	/* Copy query text, truncating if necessary */
	query_text = queryDesc->sourceText;
	qlen = strlen(query_text);
	if (qlen >= RAT_MAX_QUERY_LEN)
		qlen = RAT_MAX_QUERY_LEN - 1;
	memcpy(event.query_text, query_text, qlen);
	event.query_text[qlen] = '\0';
	event.query_len = qlen;

	/* Push into ring buffer (non-blocking) */
	rat_ring_buffer_push(&event);

	/* Increment total event counter */
	pg_atomic_fetch_add_u64(&rat_shared_state->total_events, 1);

	/* Wake the flusher BGW if it's sleeping */
	if (rat_shared_state->bgw_latch)
		SetLatch(rat_shared_state->bgw_latch);
}

/*
 * rat_ProcessUtility
 *
 * Called from the ProcessUtility hook when capture is active.
 * Captures utility statements, with special handling for transaction
 * control commands.
 */
void
rat_ProcessUtility(PlannedStmt *pstmt,
				   const char *queryString,
				   bool readOnlyTree,
				   ProcessUtilityContext context,
				   ParamListInfo params,
				   QueryEnvironment *queryEnv,
				   DestReceiver *dest,
				   QueryCompletion *qc)
{
	RatEvent	event;
	Node	   *parsetree = pstmt->utilityStmt;
	const char *query_text;
	int			qlen;

	if (queryString == NULL)
		return;

	/* Time the utility execution is handled by the caller hook wrapper,
	 * so we just capture the event with a timestamp. Note: the actual utility
	 * hasn't executed yet at this point, so duration will be 0 for the
	 * pre-execution hook. We still capture it for replay ordering. */

	memset(&event, 0, sizeof(RatEvent));

	event.timestamp = GetCurrentTimestamp();
	event.session_id = rat_compute_session_id();

	{
		LocalTransactionId lxid = MyProc ? MyProc->vxid.lxid : InvalidLocalTransactionId;

		event.transaction_id = (uint32) lxid;
	}

	/* Classify the event type */
	if (IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *tstmt = (TransactionStmt *) parsetree;

		switch (tstmt->kind)
		{
			case TRANS_STMT_BEGIN:
			case TRANS_STMT_START:
				event.event_type = RAT_EVENT_TXN_BEGIN;
				break;
			case TRANS_STMT_COMMIT:
				event.event_type = RAT_EVENT_TXN_COMMIT;
				break;
			case TRANS_STMT_ROLLBACK:
				event.event_type = RAT_EVENT_TXN_ROLLBACK;
				break;
			default:
				event.event_type = RAT_EVENT_UTILITY;
				break;
		}
	}
	else
	{
		event.event_type = RAT_EVENT_UTILITY;
	}

	event.duration_ms = 0.0;	/* pre-execution capture */
	event.rows = 0;
	event.shared_blks_hit = 0;
	event.shared_blks_read = 0;

	/* Copy query text */
	query_text = queryString;
	qlen = strlen(query_text);
	if (qlen >= RAT_MAX_QUERY_LEN)
		qlen = RAT_MAX_QUERY_LEN - 1;
	memcpy(event.query_text, query_text, qlen);
	event.query_text[qlen] = '\0';
	event.query_len = qlen;

	/* Push into ring buffer */
	rat_ring_buffer_push(&event);

	/* Increment total counter */
	pg_atomic_fetch_add_u64(&rat_shared_state->total_events, 1);

	/* Wake flusher */
	if (rat_shared_state->bgw_latch)
		SetLatch(rat_shared_state->bgw_latch);
}
