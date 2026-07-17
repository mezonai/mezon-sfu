#ifndef SFU_MEMORY_POOL_H
#define SFU_MEMORY_POOL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Fixed-capacity slab pool with a lock-free MPMC free list.
 *
 * Alloc happens on the dispatcher core; free can happen on *any* worker
 * core (a packet sent to a subscriber is released from whichever core's
 * send ring produced the completion). The free list therefore has to be
 * safe under concurrent multi-producer/multi-consumer push/pop.
 *
 * The free list is a Treiber stack over an index array, with the top
 * pointer packed as (tag:32 | index:32) in a single 64-bit word so a
 * single CMPXCHG handles both the index and an ABA-guard tag atomically.
 * The tag increments on every successful pop, which is sufficient to
 * defeat ABA within the lifetime any single thread could stall for.
 */

#define SFU_POOL_EMPTY_INDEX 0xFFFFFFFFu

typedef struct sfu_pool {
  uint8_t *slab;         /* contiguous backing storage: capacity * slot_size */
  uint32_t *next;        /* intrusive free-list links, one per slot          */
  _Atomic uint64_t head; /* packed (tag << 32) | index, SFU_POOL_EMPTY_INDEX if empty */

  uint32_t slot_size;
  uint32_t capacity;
  _Atomic uint32_t in_use; /* diagnostic counter, not used for correctness */
} sfu_pool_t;

/* Initializes a pool with `capacity` slots of `slot_size` bytes each.
 * Returns 0 on success, -1 on allocation failure. */
int sfu_pool_init(sfu_pool_t *pool, uint32_t capacity, uint32_t slot_size);
void sfu_pool_destroy(sfu_pool_t *pool);

/* Returns a pointer to a free slot and writes its index to *out_index,
 * or NULL if the pool is exhausted. Safe from any thread. */
void *sfu_pool_alloc(sfu_pool_t *pool, uint32_t *out_index);

/* Returns a slot (by index) to the free list. Safe from any thread,
 * including a thread different from the one that allocated it. */
void sfu_pool_free(sfu_pool_t *pool, uint32_t index);

/* Raw pointer lookup for an already-known index (e.g. from a CQE's
 * buffer id) without going through alloc. */
static inline void *sfu_pool_slot(sfu_pool_t *pool, uint32_t index) { return pool->slab + (size_t)index * pool->slot_size; }

#endif /* SFU_MEMORY_POOL_H */
