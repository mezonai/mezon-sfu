#include "memory/pool.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  sfu_pool_t pool;
  assert(sfu_pool_init(&pool, 8, 64) == 0);

  void *slots[8];
  uint32_t idx[8];

  /* Exhaust the pool. */
  for (int i = 0; i < 8; i++) {
    slots[i] = sfu_pool_alloc(&pool, &idx[i]);
    assert(slots[i] != NULL);
    memset(slots[i], 0xAB, 64);
  }

  /* Pool should now be exhausted. */
  uint32_t extra_idx;
  assert(sfu_pool_alloc(&pool, &extra_idx) == NULL);

  /* Free half, then confirm exactly that many can be re-allocated. */
  for (int i = 0; i < 4; i++) {
    sfu_pool_free(&pool, idx[i]);
  }
  for (int i = 0; i < 4; i++) {
    uint32_t reused_idx;
    void *p = sfu_pool_alloc(&pool, &reused_idx);
    assert(p != NULL);
  }
  assert(sfu_pool_alloc(&pool, &extra_idx) == NULL);

  sfu_pool_destroy(&pool);
  printf("test_memory_pool: OK\n");
  return 0;
}
