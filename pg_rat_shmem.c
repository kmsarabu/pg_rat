/*-------------------------------------------------------------------------
 *
 * pg_rat_shmem.c
 *		PostgreSQL Real Application Testing - shared memory management
 *
 * Implements the shared memory ring buffer for captured events.
 * The ring buffer uses a spinlock for minimal contention on the hot path.
 *
 * Design rationale:
 * - Fixed-size circular buffer avoids dynamic memory allocation.
 * - SpinLock (not LWLock) because we hold it for only a single memcpy
 *   of ~4KB, which is well within the spinlock hold time guidelines.
 * - If the buffer is full, we drop the event and increment a counter
 *   rather than blocking the query execution path.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"

#include "pg_rat.h"

/*
 * rat_shmem_size
 *
 * Compute the total shared memory needed for the ring buffer.
 */
Size
rat_shmem_size(void)
{
	Size		size;

	/* Base struct size (without flexible array) */
	size = offsetof(RatRingBuffer, events);
	/* Add space for the event array */
	size = add_size(size, mul_size(sizeof(RatEvent), rat_ring_buffer_size));

	return size;
}

/*
 * rat_shmem_request
 *
 * Called during postmaster startup to register shared memory needs.
 */
void
rat_shmem_request(void *arg)
{
	/* Request the shared state struct */
	ShmemRequestStruct(.name = "pg_rat",
					   .size = sizeof(RatSharedState),
					   .ptr = (void **) &rat_shared_state);

	/* Request the ring buffer using ShmemRequestStruct */
	ShmemRequestStruct(.name = "pg_rat ring buffer",
					   .size = rat_shmem_size(),
					   .ptr = (void **) &rat_ring_buffer);
}

/*
 * rat_shmem_init
 *
 * Called after shared memory is allocated to initialize our structures.
 */
void
rat_shmem_init(void *arg)
{
	int			tranche_id;

	Assert(!IsUnderPostmaster);

	/* Initialize the shared state (already allocated by ShmemRequestStruct) */
	tranche_id = LWLockNewTrancheId("pg_rat");
	LWLockInitialize(&rat_shared_state->lock.lock, tranche_id);

	rat_shared_state->capture_active = false;
	memset(rat_shared_state->capture_name, 0, RAT_CAPTURE_NAME_LEN);
	rat_shared_state->capture_start = 0;
	pg_atomic_init_u64(&rat_shared_state->total_events, 0);
	rat_shared_state->bgw_latch = NULL;

	rat_shared_state->replay_active = false;
	rat_shared_state->replay_sessions = 0;
	pg_atomic_init_u64(&rat_shared_state->replay_events_done, 0);
	pg_atomic_init_u64(&rat_shared_state->replay_events_total, 0);
	pg_atomic_init_u64(&rat_shared_state->replay_errors, 0);

	/* Initialize the ring buffer (already allocated by ShmemRequestStruct) */
	SpinLockInit(&rat_ring_buffer->lock);
	rat_ring_buffer->head = 0;
	rat_ring_buffer->tail = 0;
	rat_ring_buffer->capacity = rat_ring_buffer_size;
	pg_atomic_init_u64(&rat_ring_buffer->dropped_events, 0);
	memset(rat_ring_buffer->events, 0,
		   sizeof(RatEvent) * rat_ring_buffer_size);
}

/*
 * rat_ring_buffer_push
 *
 * Insert an event into the ring buffer. This is called from the
 * query execution hot path and must be as fast as possible.
 *
 * Returns true if the event was inserted, false if the buffer was
 * full (event is dropped in that case).
 */
bool
rat_ring_buffer_push(RatEvent *event)
{
	int			next_head;
	bool		inserted = false;

	if (rat_ring_buffer == NULL)
		return false;

	SpinLockAcquire(&rat_ring_buffer->lock);

	next_head = (rat_ring_buffer->head + 1) % rat_ring_buffer->capacity;

	if (next_head != rat_ring_buffer->tail)
	{
		/* Buffer has space -- copy the event in */
		memcpy(&rat_ring_buffer->events[rat_ring_buffer->head],
			   event, sizeof(RatEvent));
		rat_ring_buffer->head = next_head;
		inserted = true;
	}
	else
	{
		/* Buffer is full -- drop the event */
		pg_atomic_fetch_add_u64(&rat_ring_buffer->dropped_events, 1);
	}

	SpinLockRelease(&rat_ring_buffer->lock);

	return inserted;
}

/*
 * rat_ring_buffer_drain
 *
 * Bulk-read up to max_events from the ring buffer into out_buf.
 * Called by the background worker flusher.
 *
 * Returns the number of events drained.
 */
int
rat_ring_buffer_drain(RatEvent *out_buf, int max_events)
{
	int			count = 0;

	if (rat_ring_buffer == NULL)
		return 0;

	SpinLockAcquire(&rat_ring_buffer->lock);

	while (count < max_events && rat_ring_buffer->tail != rat_ring_buffer->head)
	{
		memcpy(&out_buf[count],
			   &rat_ring_buffer->events[rat_ring_buffer->tail],
			   sizeof(RatEvent));
		rat_ring_buffer->tail = (rat_ring_buffer->tail + 1) % rat_ring_buffer->capacity;
		count++;
	}

	SpinLockRelease(&rat_ring_buffer->lock);

	return count;
}
