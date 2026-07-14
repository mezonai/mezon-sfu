#ifndef SFU_UTIL_RINGBUFFER_H
#define SFU_UTIL_RINGBUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "sfu/config.h"

/*
 * Single-producer/single-consumer lock-free ring buffer of void* slots.
 * Used for the dispatcher-core -> worker-core packet handoff: exactly one
 * dispatcher thread pushes, exactly one worker thread pops. If you need
 * multi-producer or multi-consumer semantics, use N separate SPSC rings
 * (one per producer/consumer pair) rather than adding locking here --
 * that's what keeps this fast.
 *
 * head/tail are padded onto separate cache lines to avoid false sharing
 * between the producer and consumer, since they're written by different
 * cores on every operation.
 */
typedef struct sfu_spsc_ring {
  void **slots;
  uint32_t capacity; /* power of two */
  uint32_t mask;

  _Atomic uint32_t head __attribute__((
      aligned(SFU_CACHELINE_SIZE))); /* consumer reads here, producer writes */
  _Atomic uint32_t tail __attribute__((
      aligned(SFU_CACHELINE_SIZE))); /* producer reads here, consumer writes */
} sfu_spsc_ring_t;

int sfu_spsc_ring_init(sfu_spsc_ring_t *ring, uint32_t capacity_pow2);
void sfu_spsc_ring_destroy(sfu_spsc_ring_t *ring);

/* Producer side only. Returns false if the ring is full (backpressure --
 * caller must drop or stall, never spin-wait on the hot path). */
bool sfu_spsc_ring_push(sfu_spsc_ring_t *ring, void *item);

/* Consumer side only. Returns false if the ring is empty. */
bool sfu_spsc_ring_pop(sfu_spsc_ring_t *ring, void **out_item);

static inline uint32_t sfu_spsc_ring_size(const sfu_spsc_ring_t *ring) {
  uint32_t head = atomic_load_explicit((_Atomic uint32_t *)&ring->head,
                                       memory_order_relaxed);
  uint32_t tail = atomic_load_explicit((_Atomic uint32_t *)&ring->tail,
                                       memory_order_relaxed);
  return (tail - head) & ring->mask;
}

#endif /* SFU_UTIL_RINGBUFFER_H */
