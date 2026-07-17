#include "util/ringbuffer.h"
#include "util/alloc.h"

#include <string.h>

/*
 * head/tail are monotonically increasing counters (never wrapped
 * directly); only array indexing wraps them via `& mask`. This lets us
 * distinguish full vs. empty without sacrificing a slot:
 *   empty: tail == head
 *   full:  tail - head == capacity
 */

int sfu_spsc_ring_init(sfu_spsc_ring_t *ring, uint32_t capacity_pow2) {
  if (capacity_pow2 == 0 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
    return -1; /* must be a power of two */
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
    return false; /* full */
  }

  ring->slots[tail & ring->mask] = item;

  /* release: publish the slot write before advancing tail, so the
   * consumer's acquire-load of tail is guaranteed to see the item. */
  atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
  return true;
}

bool sfu_spsc_ring_pop(sfu_spsc_ring_t *ring, void **out_item) {
  uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
  uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

  if (head == tail) {
    return false; /* empty */
  }

  *out_item = ring->slots[head & ring->mask];

  atomic_store_explicit(&ring->head, head + 1, memory_order_release);
  return true;
}
