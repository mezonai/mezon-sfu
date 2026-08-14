#ifndef SFU_MEMORY_POOL_H
#define SFU_MEMORY_POOL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_POOL_EMPTY_INDEX 0xFFFFFFFFu

typedef struct sfu_pool {
  uint8_t *slab;
  uint32_t *next;
  _Atomic uint64_t head;

  uint32_t slot_size;
  uint32_t capacity;
  _Atomic uint32_t in_use;
  _Atomic uint32_t high_water;
  _Atomic uint64_t alloc_failures;
} sfu_pool_t;

int sfu_pool_init(sfu_pool_t *pool, uint32_t capacity, uint32_t slot_size);
void sfu_pool_destroy(sfu_pool_t *pool);
void *sfu_pool_alloc(sfu_pool_t *pool, uint32_t *out_index);
void sfu_pool_free(sfu_pool_t *pool, uint32_t index);
uint32_t sfu_pool_in_use(const sfu_pool_t *pool);
uint32_t sfu_pool_high_water(const sfu_pool_t *pool);
uint64_t sfu_pool_alloc_failures(const sfu_pool_t *pool);

static inline void *sfu_pool_slot(sfu_pool_t *pool, uint32_t index) { return pool->slab + (size_t)index * pool->slot_size; }

#endif /* SFU_MEMORY_POOL_H */
