#ifndef SFU_MEMORY_WORKER_PACKET_ARENA_H
#define SFU_MEMORY_WORKER_PACKET_ARENA_H

#include <stdint.h>

#include "sfu/packet.h"

typedef struct sfu_worker_packet_arena {
  sfu_packet_t *packets;
  uint8_t *data;
  uint32_t capacity;
  uint32_t buf_size;
  uint32_t free_head;
  uint32_t in_use;
  uint32_t high_water;
  uint64_t allocated;
  uint64_t recycled;
  uint64_t exhausted;
} sfu_worker_packet_arena_t;

int sfu_worker_packet_arena_init(sfu_worker_packet_arena_t *arena, uint32_t capacity, uint32_t buf_size);
void sfu_worker_packet_arena_destroy(sfu_worker_packet_arena_t *arena);
sfu_packet_t *sfu_worker_packet_arena_alloc(sfu_worker_packet_arena_t *arena);
void sfu_worker_packet_arena_free(sfu_packet_t *pkt);

#endif /* SFU_MEMORY_WORKER_PACKET_ARENA_H */
