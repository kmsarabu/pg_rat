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
	RatEventMeta meta;
	const char *query_text;
	int			qlen;

	/* Safety: skip if no query text available */
	if (queryDesc->sourceText == NULL)
		return;

	meta.timestamp = GetCurrentTimestamp();
	meta.session_id = rat_compute_session_id();

	/* Use the current virtual transaction ID as our transaction_id */
	{
		LocalTransactionId lxid = MyProc ? MyProc->vxid.lxid : InvalidLocalTransactionId;
		meta.transaction_id = (uint32) lxid;
	}

	meta.event_type = RAT_EVENT_QUERY_END;

	/* Timing: if instrumentation is available, use it */
	if (queryDesc->query_instr)
	{
		meta.duration_ms = INSTR_TIME_GET_MILLISEC(queryDesc->query_instr->total);
		meta.shared_blks_hit = queryDesc->query_instr->bufusage.shared_blks_hit;
		meta.shared_blks_read = queryDesc->query_instr->bufusage.shared_blks_read;
	}
	else
	{
		meta.duration_ms = 0.0;
		meta.shared_blks_hit = 0;
		meta.shared_blks_read = 0;
	}

	meta.rows = queryDesc->estate->es_total_processed;

	/* Copy query text pointer and length */
	query_text = queryDesc->sourceText;
	qlen = (int) strlen(query_text);
	if (qlen >= RAT_MAX_QUERY_LEN)
		qlen = RAT_MAX_QUERY_LEN - 1;

	/* Push into ring buffer directly */
	rat_ring_buffer_push(&meta, query_text, qlen);

	/* Increment total event counter */
	pg_atomic_fetch_add_u64(&rat_shared_state->total_events, 1);
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
	RatEventMeta meta;
	Node	   *parsetree = pstmt->utilityStmt;
	const char *query_text;
	int			qlen;

	if (queryString == NULL)
		return;

	/* Time the utility execution is handled by the caller hook wrapper,
	 * so we just capture the event with a timestamp. Note: the actual utility
	 * hasn't executed yet at this point, so duration will be 0 for the
	 * pre-execution hook. We still capture it for replay ordering. */

	meta.timestamp = GetCurrentTimestamp();
	meta.session_id = rat_compute_session_id();

	{
		LocalTransactionId lxid = MyProc ? MyProc->vxid.lxid : InvalidLocalTransactionId;
		meta.transaction_id = (uint32) lxid;
	}

	/* Classify the event type */
	if (IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *tstmt = (TransactionStmt *) parsetree;

		switch (tstmt->kind)
		{
			case TRANS_STMT_BEGIN:
			case TRANS_STMT_START:
				meta.event_type = RAT_EVENT_TXN_BEGIN;
				break;
			case TRANS_STMT_COMMIT:
				meta.event_type = RAT_EVENT_TXN_COMMIT;
				break;
			case TRANS_STMT_ROLLBACK:
				meta.event_type = RAT_EVENT_TXN_ROLLBACK;
				break;
			default:
				meta.event_type = RAT_EVENT_UTILITY;
				break;
		}
	}
	else
	{
		meta.event_type = RAT_EVENT_UTILITY;
	}

	meta.duration_ms = 0.0;	/* pre-execution capture */
	meta.rows = 0;
	meta.shared_blks_hit = 0;
	meta.shared_blks_read = 0;

	/* Copy query text pointer and length */
	query_text = queryString;
	qlen = (int) strlen(query_text);
	if (qlen >= RAT_MAX_QUERY_LEN)
		qlen = RAT_MAX_QUERY_LEN - 1;

	/* Push into ring buffer */
	rat_ring_buffer_push(&meta, query_text, qlen);

	/* Increment total counter */
	pg_atomic_fetch_add_u64(&rat_shared_state->total_events, 1);
}
