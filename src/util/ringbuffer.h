#ifndef SFU_UTIL_RINGBUFFER_H
#define SFU_UTIL_RINGBUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "sfu/config.h"

typedef struct sfu_spsc_ring {
  void **slots;
  uint32_t capacity; /* power of two */
  uint32_t mask;

  _Atomic uint32_t head __attribute__((aligned(SFU_CACHELINE_SIZE)));
  _Atomic uint32_t tail __attribute__((aligned(SFU_CACHELINE_SIZE)));
  _Atomic uint32_t high_water;
  _Atomic uint64_t push_failures;
} sfu_spsc_ring_t;

int sfu_spsc_ring_init(sfu_spsc_ring_t *ring, uint32_t capacity_pow2);
void sfu_spsc_ring_destroy(sfu_spsc_ring_t *ring);
bool sfu_spsc_ring_push(sfu_spsc_ring_t *ring, void *item);
bool sfu_spsc_ring_pop(sfu_spsc_ring_t *ring, void **out_item);

static inline uint32_t sfu_spsc_ring_size(const sfu_spsc_ring_t *ring) {
  uint32_t head = atomic_load_explicit((_Atomic uint32_t *)&ring->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit((_Atomic uint32_t *)&ring->tail, memory_order_relaxed);
  return (tail - head) & ring->mask;
}

static inline uint32_t sfu_spsc_ring_high_water(const sfu_spsc_ring_t *ring) {
  return atomic_load_explicit((_Atomic uint32_t *)&ring->high_water, memory_order_relaxed);
}

static inline uint64_t sfu_spsc_ring_push_failures(const sfu_spsc_ring_t *ring) {
  return atomic_load_explicit((_Atomic uint64_t *)&ring->push_failures, memory_order_relaxed);
}

#endif /* SFU_UTIL_RINGBUFFER_H */
