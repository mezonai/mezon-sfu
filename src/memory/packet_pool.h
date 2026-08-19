#ifndef SFU_MEMORY_PACKET_POOL_H
#define SFU_MEMORY_PACKET_POOL_H

#include "memory/pool.h"
#include "sfu/packet.h"

typedef struct sfu_packet_pool {
  sfu_pool_t meta;
  sfu_pool_t data;
} sfu_packet_pool_t;

int sfu_packet_pool_init(sfu_packet_pool_t *pp, uint32_t capacity, uint32_t buf_size);
void sfu_packet_pool_destroy(sfu_packet_pool_t *pp);
sfu_packet_t *sfu_packet_pool_alloc(sfu_packet_pool_t *pp);
void sfu_packet_pool_free(sfu_packet_pool_t *pp, sfu_packet_t *pkt);
sfu_packet_t *sfu_packet_pool_alloc_meta(sfu_packet_pool_t *pp);
void sfu_packet_pool_free_meta(sfu_packet_pool_t *pp, sfu_packet_t *pkt);

#endif /* SFU_MEMORY_PACKET_POOL_H */
