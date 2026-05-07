/*-------------------------------------------------------------------------
 *
 * pg_rat.h
 *		PostgreSQL Real Application Testing - shared header
 *
 * Provides workload capture, replay, SQL Performance Analysis (SPA),
 * and HTML report generation for database migration/upgrade testing.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_RAT_H
#define PG_RAT_H

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/timestamp.h"

#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"
#include "tcop/utility.h"
#include "utils/queryenvironment.h"

/* Forward declarations for remaining dependencies */
typedef struct QueryDesc QueryDesc;
typedef struct QueryCompletion QueryCompletion;

/* ----------------
 * Constants
 * ----------------
 */
#define RAT_MAX_QUERY_LEN		1024	/* max query text length per event */
#define RAT_DEFAULT_RING_SIZE	8192	/* default ring buffer slot count */
#define RAT_MAX_RING_SIZE		65536	/* maximum ring buffer slot count */
#define RAT_CAPTURE_NAME_LEN	128		/* max capture session name */
#define RAT_CAPTURE_DIR			"pg_rat_capture"
#define RAT_FILE_ROTATE_SIZE	(64 * 1024 * 1024)	/* 64 MB file rotation */
#define RAT_BGW_NAPTIME_MS		100		/* flusher wake interval */

/* ----------------
 * Event Types
 * ----------------
 */
typedef enum RatEventType
{
	RAT_EVENT_QUERY_END = 0,	/* DML/SELECT completed */
	RAT_EVENT_UTILITY,			/* utility statement */
	RAT_EVENT_TXN_BEGIN,		/* BEGIN */
	RAT_EVENT_TXN_COMMIT,		/* COMMIT */
	RAT_EVENT_TXN_ROLLBACK		/* ROLLBACK */
} RatEventType;

typedef enum RatSlotStatus
{
	RAT_SLOT_EMPTY = 0,
	RAT_SLOT_BUSY,
	RAT_SLOT_READY
} RatSlotStatus;

/* ----------------
 * Captured Event
 *
 * This struct is placed directly into the shared memory ring buffer.
 * No pointers, no palloc — everything is fixed-size for atomic operations.
 * ----------------
 */
typedef struct RatEvent
{
	pg_atomic_uint32 status;				/* RAT_SLOT_xxx */
	TimestampTz timestamp;					/* event timestamp, usec precision */
	uint32		session_id;					/* hash of PID + session start */
	uint32		transaction_id;				/* virtual transaction id */
	RatEventType event_type;				/* RatEventType */
	double		duration_ms;				/* execution time in milliseconds */
	int64		rows;						/* rows affected/returned */
	int64		shared_blks_hit;			/* shared buffer hits */
	int64		shared_blks_read;			/* shared disk blocks read */
	int32		query_len;					/* actual length of query_text */
	char		query_text[RAT_MAX_QUERY_LEN]; /* truncated query text */
} RatEvent;

/* Metadata only struct for efficient passing to push function */
typedef struct RatEventMeta
{
	TimestampTz timestamp;
	uint32		session_id;
	uint32		transaction_id;
	RatEventType event_type;
	double		duration_ms;
	int64		rows;
	int64		shared_blks_hit;
	int64		shared_blks_read;
} RatEventMeta;

/* ----------------
 * Ring Buffer
 *
 * Fixed-size circular buffer of RatEvent structs in shared memory.
 * Uses atomic head/tail for lock-free concurrency.
 * ----------------
 */
typedef struct RatRingBuffer
{
	pg_atomic_uint64 head;				/* Next sequence number to fill */
	pg_atomic_uint64 tail;				/* Next sequence number to drain */
	int			capacity;				/* total slot count */
	pg_atomic_uint64 dropped_events;	/* events dropped due to full buffer */
	RatEvent	events[FLEXIBLE_ARRAY_MEMBER];
} RatRingBuffer;

/* ----------------
 * Shared State
 *
 * Global state for the pg_rat module, stored in shared memory.
 * ----------------
 */
typedef struct RatSharedState
{
	LWLockPadded lock;			/* protects non-ring-buffer state */
	bool		capture_active; /* is capture currently running? */
	char		capture_name[RAT_CAPTURE_NAME_LEN]; /* current capture session */
	TimestampTz capture_start;	/* when capture started */
	pg_atomic_uint64 total_events;	/* total events captured */
	struct Latch *bgw_latch;		/* latch to wake the flusher BGW */

	/* Replay state */
	bool		replay_active;
	int			replay_sessions;	/* number of sessions being replayed */
	pg_atomic_uint64 replay_events_done;
	pg_atomic_uint64 replay_events_total;
	pg_atomic_uint64 replay_errors;
} RatSharedState;

/* ----------------
 * GUC variables (defined in pg_rat.c)
 * ----------------
 */
extern bool rat_enabled;
extern int	rat_ring_buffer_size;
extern int	rat_max_query_length;
extern char *rat_capture_directory;

/* ----------------
 * Global shared state pointers (defined in pg_rat.c)
 * ----------------
 */
extern RatSharedState *rat_shared_state;
extern RatRingBuffer *rat_ring_buffer;

/* ----------------
 * Capture functions (pg_rat_capture.c)
 *
 * Declarations are not in this header to avoid requiring
 * executor/utility headers in every translation unit.
 * See pg_rat.c and pg_rat_capture.c for the actual prototypes.
 * ----------------
 */

/* ----------------
 * Capture functions (pg_rat_capture.c)
 * ----------------
 */
extern void rat_ExecutorEnd(QueryDesc *queryDesc);
extern void rat_ProcessUtility(PlannedStmt *pstmt,
							   const char *queryString,
							   bool readOnlyTree,
							   ProcessUtilityContext context,
							   ParamListInfo params,
							   QueryEnvironment *queryEnv,
							   DestReceiver *dest,
							   QueryCompletion *qc);

/* ----------------
 * Shared memory functions (pg_rat_shmem.c)
 * ----------------
 */
extern Size rat_shmem_size(void);
extern void rat_shmem_request(void *arg);
extern void rat_shmem_init(void *arg);
extern bool rat_ring_buffer_push(RatEventMeta *meta, const char *query_text, int query_len);
extern int	rat_ring_buffer_drain(RatEvent *out_buf, int max_events);

/* ----------------
 * Background worker functions (pg_rat_bgworker.c)
 * ----------------
 */
extern void rat_bgworker_main(Datum main_arg);

/* ----------------
 * Replay functions (pg_rat_replayer.c)
 * ----------------
 */
extern void rat_replay_coordinator_main(Datum main_arg);

/* ----------------
 * Utility helpers
 * ----------------
 */
static inline uint32
rat_compute_session_id(void)
{
	uint32		h;

	h = (uint32) MyProcPid;
	h ^= (uint32) (MyStartTimestamp >> 32);
	h ^= (uint32) MyStartTimestamp;
	/* Simple hash mixing */
	h = (h ^ (h >> 16)) * 0x45d9f3b;
	h = (h ^ (h >> 16)) * 0x45d9f3b;
	h = h ^ (h >> 16);
	return h;
}

#endif							/* PG_RAT_H */
