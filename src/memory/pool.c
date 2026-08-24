#include "memory/pool.h"
#include "util/alloc.h"

#include <string.h>

static inline uint64_t pack(uint32_t tag, uint32_t index) { return ((uint64_t)tag << 32) | (uint64_t)index; }
static inline uint32_t unpack_tag(uint64_t v) { return (uint32_t)(v >> 32); }
static inline uint32_t unpack_index(uint64_t v) { return (uint32_t)(v & 0xFFFFFFFFu); }

int sfu_pool_init(sfu_pool_t *pool, uint32_t capacity, uint32_t slot_size) {
  memset(pool, 0, sizeof(*pool));

  pool->slab = SFU_ALIGNED_ALLOC(64, (size_t)capacity * slot_size);
  if (!pool->slab) {
    return -1;
  }

  pool->next = SFU_MALLOC(sizeof(*pool->next) * capacity);
  if (!pool->next) {
    SFU_FREE(pool->slab);
    pool->slab = NULL;
    return -1;
  }

  pool->slot_size = slot_size;
  pool->capacity = capacity;
  atomic_init(&pool->in_use, 0);
  atomic_init(&pool->high_water, 0);
  atomic_init(&pool->alloc_failures, 0);

  for (uint32_t i = 0; i < capacity; i++) {
    atomic_init(&pool->next[i], (i + 1 < capacity) ? (i + 1) : SFU_POOL_EMPTY_INDEX);
  }
  atomic_init(&pool->head, pack(0, 0));

  return 0;
}

void sfu_pool_destroy(sfu_pool_t *pool) {
  SFU_FREE(pool->slab);
  SFU_FREE(pool->next);
  pool->slab = NULL;
  pool->next = NULL;
}

void *sfu_pool_alloc(sfu_pool_t *pool, uint32_t *out_index) {
  uint64_t old_head = atomic_load_explicit(&pool->head, memory_order_acquire);

  for (;;) {
    uint32_t idx = unpack_index(old_head);
    if (idx == SFU_POOL_EMPTY_INDEX) {
      atomic_fetch_add_explicit(&pool->alloc_failures, 1, memory_order_relaxed);
      return NULL; /* pool exhausted */
    }

    uint32_t next_idx = atomic_load_explicit(&pool->next[idx], memory_order_relaxed);
    uint32_t tag = unpack_tag(old_head);
    uint64_t new_head = pack(tag + 1, next_idx);

    if (atomic_compare_exchange_weak_explicit(&pool->head, &old_head, new_head, memory_order_acq_rel, memory_order_acquire)) {
      uint32_t in_use = atomic_fetch_add_explicit(&pool->in_use, 1, memory_order_relaxed) + 1;
      uint32_t high = atomic_load_explicit(&pool->high_water, memory_order_relaxed);
      while (in_use > high && !atomic_compare_exchange_weak_explicit(&pool->high_water, &high, in_use, memory_order_relaxed, memory_order_relaxed)) {
      }
      if (out_index) {
        *out_index = idx;
      }
      return sfu_pool_slot(pool, idx);
    }
  }
}

void sfu_pool_free(sfu_pool_t *pool, uint32_t index) {
  uint64_t old_head = atomic_load_explicit(&pool->head, memory_order_acquire);

  for (;;) {
    uint32_t old_idx = unpack_index(old_head);
    uint32_t tag = unpack_tag(old_head);

    atomic_store_explicit(&pool->next[index], old_idx, memory_order_relaxed);

    uint64_t new_head = pack(tag + 1, index);

    if (atomic_compare_exchange_weak_explicit(&pool->head, &old_head, new_head, memory_order_release, memory_order_relaxed)) {
      atomic_fetch_sub_explicit(&pool->in_use, 1, memory_order_relaxed);
      return;
    }
  }
}

uint32_t sfu_pool_in_use(const sfu_pool_t *pool) { return atomic_load_explicit(&pool->in_use, memory_order_relaxed); }

uint32_t sfu_pool_high_water(const sfu_pool_t *pool) { return atomic_load_explicit(&pool->high_water, memory_order_relaxed); }

uint64_t sfu_pool_alloc_failures(const sfu_pool_t *pool) { return atomic_load_explicit(&pool->alloc_failures, memory_order_relaxed); }
