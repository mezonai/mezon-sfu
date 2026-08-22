#include "memory/worker_packet_arena.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#include "memory/refcount.h"
#include "util/alloc.h"

#define SFU_WORKER_ARENA_FREE_NONE UINT32_MAX

int sfu_worker_packet_arena_init(sfu_worker_packet_arena_t *arena, uint32_t capacity, uint32_t buf_size) {
  if (!arena || capacity == 0 || buf_size == 0) {
    return -1;
  }

  memset(arena, 0, sizeof(*arena));
  arena->packets = SFU_CALLOC(capacity, sizeof(*arena->packets));
  arena->data = SFU_ALIGNED_ALLOC(64, (size_t)capacity * buf_size);
  if (!arena->packets || !arena->data) {
    SFU_FREE(arena->data);
    SFU_FREE(arena->packets);
    memset(arena, 0, sizeof(*arena));
    return -1;
  }

  arena->capacity = capacity;
  arena->buf_size = buf_size;
  arena->free_head = 0;
  for (uint32_t i = 0; i < capacity; i++) {
    sfu_packet_t *pkt = &arena->packets[i];
    pkt->pool_index = i;
    pkt->kbuf_index = i + 1 < capacity ? i + 1 : SFU_WORKER_ARENA_FREE_NONE;
    pkt->buf_source = SFU_BUF_SOURCE_WORKER_ARENA;
    pkt->buf_owner = arena;
  }
  return 0;
}

void sfu_worker_packet_arena_destroy(sfu_worker_packet_arena_t *arena) {
  if (!arena) {
    return;
  }
  SFU_FREE(arena->data);
  SFU_FREE(arena->packets);
  memset(arena, 0, sizeof(*arena));
}

sfu_packet_t *sfu_worker_packet_arena_alloc(sfu_worker_packet_arena_t *arena) {
  if (!arena || !arena->packets || arena->free_head == SFU_WORKER_ARENA_FREE_NONE) {
    if (arena) {
      arena->exhausted++;
    }
    return NULL;
  }

  uint32_t index = arena->free_head;
  sfu_packet_t *pkt = &arena->packets[index];
  arena->free_head = pkt->kbuf_index;

  uint32_t prior_gen = atomic_load_explicit(&pkt->generation, memory_order_relaxed);
  memset(pkt, 0, sizeof(*pkt));
  atomic_store_explicit(&pkt->generation, prior_gen, memory_order_relaxed);
  pkt->data = arena->data + (size_t)index * arena->buf_size;
  pkt->cap = arena->buf_size;
  pkt->pool_index = index;
  pkt->buf_source = SFU_BUF_SOURCE_WORKER_ARENA;
  pkt->buf_owner = arena;
  sfu_packet_reinit(pkt);

  arena->allocated++;
  arena->in_use++;
  if (arena->in_use > arena->high_water) {
    arena->high_water = arena->in_use;
  }
  return pkt;
}

void sfu_worker_packet_arena_free(sfu_packet_t *pkt) {
  assert(pkt && pkt->buf_source == SFU_BUF_SOURCE_WORKER_ARENA);
  sfu_worker_packet_arena_t *arena = (sfu_worker_packet_arena_t *)pkt->buf_owner;
  assert(arena && pkt >= arena->packets && pkt < arena->packets + arena->capacity);
  assert(arena->in_use > 0);

  uint32_t index = pkt->pool_index;
  pkt->data = NULL;
  pkt->kbuf_index = arena->free_head;
  arena->free_head = index;
  arena->in_use--;
  arena->recycled++;
}
