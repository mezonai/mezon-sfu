#include "util/ringbuffer.h"
#include "util/alloc.h"

#include <string.h>

int sfu_spsc_ring_init(sfu_spsc_ring_t *ring, uint32_t capacity_pow2) {
  if (capacity_pow2 == 0 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
    return -1;
  }

  memset(ring, 0, sizeof(*ring));
  ring->slots = SFU_CALLOC(capacity_pow2, sizeof(void *));
  if (!ring->slots) {
    return -1;
  }

  ring->capacity = capacity_pow2;
  ring->mask = capacity_pow2 - 1;
  atomic_init(&ring->head, 0);
  atomic_init(&ring->tail, 0);
  atomic_init(&ring->high_water, 0);
  atomic_init(&ring->push_failures, 0);

  return 0;
}

void sfu_spsc_ring_destroy(sfu_spsc_ring_t *ring) {
  SFU_FREE(ring->slots);
  ring->slots = NULL;
}

bool sfu_spsc_ring_push(sfu_spsc_ring_t *ring, void *item) {
  uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
  uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

  if (tail - head >= ring->capacity) {
    atomic_fetch_add_explicit(&ring->push_failures, 1, memory_order_relaxed);
    return false;
  }

  ring->slots[tail & ring->mask] = item;

  uint32_t depth = tail - head + 1;
  uint32_t high = atomic_load_explicit(&ring->high_water, memory_order_relaxed);
  while (depth > high && !atomic_compare_exchange_weak_explicit(&ring->high_water, &high, depth, memory_order_relaxed, memory_order_relaxed)) {
  }

  atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
  return true;
}

bool sfu_spsc_ring_pop(sfu_spsc_ring_t *ring, void **out_item) {
  uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

  if (head == tail) {
    return false;
  }

  *out_item = ring->slots[head & ring->mask];

  atomic_store_explicit(&ring->head, head + 1, memory_order_release);
  return true;
}
