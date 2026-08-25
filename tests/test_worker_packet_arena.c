#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "memory/refcount.h"
#include "memory/worker_packet_arena.h"
#include "net/net.h"

static void test_fixed_capacity_and_recycling(void) {
  sfu_worker_packet_arena_t arena;
  assert(sfu_worker_packet_arena_init(&arena, 2, 256) == 0);

  sfu_packet_t *a = sfu_worker_packet_arena_alloc(&arena);
  sfu_packet_t *b = sfu_worker_packet_arena_alloc(&arena);
  assert(a != NULL && b != NULL && a != b);
  assert(a->data != b->data);
  assert(a->cap == 256 && b->cap == 256);
  assert(a->buf_source == SFU_BUF_SOURCE_WORKER_ARENA);
  assert(arena.in_use == 2 && arena.high_water == 2);
  assert(sfu_worker_packet_arena_alloc(&arena) == NULL);
  assert(arena.exhausted == 1);

  uint32_t generation = sfu_packet_generation(a);
  sfu_net_worker_release_packet(NULL, NULL, a);
  assert(arena.in_use == 1 && arena.recycled == 1);

  sfu_packet_t *reused = sfu_worker_packet_arena_alloc(&arena);
  assert(reused == a);
  assert(sfu_packet_generation(reused) == generation + 1);
  sfu_net_worker_release_packet(NULL, NULL, reused);
  sfu_net_worker_release_packet(NULL, NULL, b);
  assert(arena.in_use == 0 && arena.recycled == 3);

  sfu_worker_packet_arena_destroy(&arena);
}

static void test_retained_packet_recycles_on_final_release(void) {
  sfu_worker_packet_arena_t arena;
  assert(sfu_worker_packet_arena_init(&arena, 1, 128) == 0);

  sfu_packet_t *pkt = sfu_worker_packet_arena_alloc(&arena);
  assert(pkt != NULL);
  sfu_packet_retain(pkt, 1);
  sfu_net_worker_release_packet(NULL, NULL, pkt);
  assert(arena.in_use == 1);
  assert(sfu_worker_packet_arena_alloc(&arena) == NULL);

  sfu_net_worker_release_packet(NULL, NULL, pkt);
  assert(arena.in_use == 0);
  assert(sfu_worker_packet_arena_alloc(&arena) == pkt);
  sfu_net_worker_release_packet(NULL, NULL, pkt);

  sfu_worker_packet_arena_destroy(&arena);
}

int main(void) {
  test_fixed_capacity_and_recycling();
  test_retained_packet_recycles_on_final_release();
  printf("worker packet arena tests passed\n");
  return 0;
}
