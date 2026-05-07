/*-------------------------------------------------------------------------
 *
 * pg_rat_shmem.c
 *		PostgreSQL Real Application Testing - shared memory management
 *
 * Implements the shared memory ring buffer for captured events.
 * The ring buffer uses lock-free atomic operations for minimal
 * contention on the hot path.
 *
 * Design rationale:
 * - Fixed-size circular buffer avoids dynamic memory allocation.
 * - Atomic compare-exchange on the head pointer allows multiple
 *   backends to claim slots simultaneously without blocking.
 * - Each slot has a status field (EMPTY/BUSY/READY) for safe
 *   handoff between backends and the flusher.
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
Size rat_shmem_size(void) {
  Size size;

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
void rat_shmem_request(void *arg) {
  /* Request the shared state struct */
  ShmemRequestStruct(.name = "pg_rat", .size = sizeof(RatSharedState),
                     .ptr = (void **)&rat_shared_state);

  /* Request the ring buffer using ShmemRequestStruct */
  ShmemRequestStruct(.name = "pg_rat ring buffer", .size = rat_shmem_size(),
                     .ptr = (void **)&rat_ring_buffer);
}

/*
 * rat_shmem_init
 *
 * Called after shared memory is allocated to initialize our structures.
 */
void rat_shmem_init(void *arg) {
  int tranche_id;

  Assert(!IsUnderPostmaster);

  /* Initialize the shared state (already allocated by ShmemRequestStruct) */
  tranche_id = LWLockNewTrancheId("pg_rat");
  LWLockInitialize(&rat_shared_state->lock.lock, tranche_id);

  pg_atomic_init_u32(&rat_shared_state->capture_active, 0);
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
  pg_atomic_init_u64(&rat_ring_buffer->head, 0);
  pg_atomic_init_u64(&rat_ring_buffer->tail, 0);
  rat_ring_buffer->capacity = rat_ring_buffer_size;
  pg_atomic_init_u64(&rat_ring_buffer->dropped_events, 0);

  for (int i = 0; i < rat_ring_buffer_size; i++)
    pg_atomic_init_u32(&rat_ring_buffer->events[i].status, RAT_SLOT_EMPTY);
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
bool rat_ring_buffer_push(RatEventMeta *meta, const char *query_text,
                          int query_len) {
  uint64 head, tail;
  uint32 idx;
  RatEvent *slot;

  if (rat_ring_buffer == NULL)
    return false;

  /* Reserve a slot atomically */
  do {
    head = pg_atomic_read_u64(&rat_ring_buffer->head);
    tail = pg_atomic_read_u64(&rat_ring_buffer->tail);

    /* Check if buffer is full */
    if (head - tail >= rat_ring_buffer->capacity) {
      pg_atomic_fetch_add_u64(&rat_ring_buffer->dropped_events, 1);
      return false;
    }

  } while (
      !pg_atomic_compare_exchange_u64(&rat_ring_buffer->head, &head, head + 1));

  /* We have claimed slot 'head' */
  idx = (uint32)(head % rat_ring_buffer->capacity);
  slot = &rat_ring_buffer->events[idx];

  /* Wait if the slot hasn't been emptied by flusher yet (rare) */
  {
    int retries = 0;

    while (pg_atomic_read_u32(&slot->status) != RAT_SLOT_EMPTY) {
      if (++retries > 10000) {
        /* Flusher is stuck or too slow — drop rather than hang */
        pg_atomic_fetch_add_u64(&rat_ring_buffer->dropped_events, 1);
        return false;
      }
      SPIN_DELAY();
    }
  }

  /* Mark busy and write metadata directly to shmem */
  pg_atomic_write_u32(&slot->status, RAT_SLOT_BUSY);

  slot->timestamp = meta->timestamp;
  slot->session_id = meta->session_id;
  slot->transaction_id = meta->transaction_id;
  slot->event_type = meta->event_type;
  slot->duration_ms = meta->duration_ms;
  slot->rows = meta->rows;
  slot->shared_blks_hit = meta->shared_blks_hit;
  slot->shared_blks_read = meta->shared_blks_read;
  slot->query_len = query_len;

  /* Copy ONLY the query bytes we actually have */
  if (query_text && query_len > 0) {
    memcpy(slot->query_text, query_text, query_len);
  }
  slot->query_text[query_len] = '\0';

  /* Mark ready for flusher */
  pg_atomic_write_u32(&slot->status, RAT_SLOT_READY);

  return true;
}

/*
 * rat_ring_buffer_drain
 *
 * Bulk-read up to max_events from the ring buffer into out_buf.
 * Called by the background worker flusher.
 *
 * Returns the number of events drained.
 */
int rat_ring_buffer_drain(RatEvent *out_buf, int max_events) {
  int count = 0;
  uint64 tail;

  if (rat_ring_buffer == NULL)
    return 0;

  tail = pg_atomic_read_u64(&rat_ring_buffer->tail);

  while (count < max_events) {
    uint32 idx = (uint32)(tail % rat_ring_buffer->capacity);
    RatEvent *slot = &rat_ring_buffer->events[idx];

    /* Only drain if the slot is fully written */
    if (pg_atomic_read_u32(&slot->status) != RAT_SLOT_READY)
      break;

    /* Copy data out */
    memcpy(out_buf + count, slot, sizeof(RatEvent));

    /* Mark slot as empty for backends to reuse */
    pg_atomic_write_u32(&slot->status, RAT_SLOT_EMPTY);

    tail++;
    count++;
  }

  /* Update global tail after batching */
  if (count > 0)
    pg_atomic_write_u64(&rat_ring_buffer->tail, tail);

  return count;
}
