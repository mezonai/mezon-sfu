#ifndef SFU_MEMORY_REFCOUNT_H
#define SFU_MEMORY_REFCOUNT_H

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>

#include "sfu/packet.h"

static inline void sfu_packet_retain(sfu_packet_t *pkt, uint32_t n) {
  assert(n > 0);
  atomic_fetch_add_explicit(&pkt->refcount, n, memory_order_relaxed);
}

static inline int sfu_packet_release(sfu_packet_t *pkt) {
  uint32_t prev = atomic_fetch_sub_explicit(&pkt->refcount, 1, memory_order_acq_rel);
  assert(prev != 0 && "refcount underflow: released an already-dead packet");
  return prev == 1;
}

static inline uint32_t sfu_packet_generation(const sfu_packet_t *pkt) {
  return atomic_load_explicit((_Atomic uint32_t *)&pkt->generation, memory_order_acquire);
}

static inline void sfu_packet_reinit(sfu_packet_t *pkt) {
  uint32_t gen = atomic_load_explicit(&pkt->generation, memory_order_relaxed);
  atomic_store_explicit(&pkt->generation, gen + 1, memory_order_relaxed);
  atomic_store_explicit(&pkt->refcount, 1, memory_order_relaxed);
}

void sfu_packet_debug_dump(const sfu_packet_t *pkt);

#endif /* SFU_MEMORY_REFCOUNT_H */
