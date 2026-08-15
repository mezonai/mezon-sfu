#include "memory/packet_pool.h"
#include "memory/refcount.h"
#include "util/metrics.h"

#include <stdatomic.h>
#include <string.h>

int sfu_packet_pool_init(sfu_packet_pool_t *pp, uint32_t capacity, uint32_t buf_size) {
  if (sfu_pool_init(&pp->meta, capacity, sizeof(sfu_packet_t)) != 0) {
    return -1;
  }
  if (sfu_pool_init(&pp->data, capacity, buf_size) != 0) {
    sfu_pool_destroy(&pp->meta);
    return -1;
  }
  return 0;
}

void sfu_packet_pool_destroy(sfu_packet_pool_t *pp) {
  sfu_pool_destroy(&pp->meta);
  sfu_pool_destroy(&pp->data);
}

sfu_packet_t *sfu_packet_pool_alloc(sfu_packet_pool_t *pp) {
  uint32_t meta_idx, data_idx;

  sfu_packet_t *pkt = sfu_pool_alloc(&pp->meta, &meta_idx);
  if (!pkt) {
    sfu_metric_inc("packet_meta_pool_exhausted");
    return NULL;
  }

  void *buf = sfu_pool_alloc(&pp->data, &data_idx);
  if (!buf) {
    sfu_metric_inc("packet_pool_exhausted");
    sfu_pool_free(&pp->meta, meta_idx);
    return NULL;
  }

  uint32_t prior_gen = atomic_load_explicit(&pkt->generation, memory_order_relaxed);
  memset(pkt, 0, sizeof(*pkt));
  atomic_store_explicit(&pkt->generation, prior_gen, memory_order_relaxed);
  pkt->data = buf;
  pkt->cap = (uint32_t)pp->data.slot_size;
  pkt->pool_index = meta_idx;
  pkt->kbuf_index = data_idx;
  pkt->buf_source = SFU_BUF_SOURCE_POOL;

  sfu_packet_reinit(pkt); /* refcount=1, generation++ */

  return pkt;
}

void sfu_packet_pool_free(sfu_packet_pool_t *pp, sfu_packet_t *pkt) {
  uint32_t data_idx = pkt->kbuf_index;
  uint32_t meta_idx = pkt->pool_index;

  pkt->data = NULL; /* guard against use-after-free within this process */

  sfu_pool_free(&pp->data, data_idx);
  sfu_pool_free(&pp->meta, meta_idx);
}

sfu_packet_t *sfu_packet_pool_alloc_meta(sfu_packet_pool_t *pp) {
  uint32_t meta_idx;
  sfu_packet_t *pkt = sfu_pool_alloc(&pp->meta, &meta_idx);
  if (!pkt) {
    sfu_metric_inc("packet_meta_pool_exhausted");
    return NULL;
  }

  uint32_t prior_gen = atomic_load_explicit(&pkt->generation, memory_order_relaxed);
  memset(pkt, 0, sizeof(*pkt));
  atomic_store_explicit(&pkt->generation, prior_gen, memory_order_relaxed);
  pkt->pool_index = meta_idx;
  pkt->buf_source = SFU_BUF_SOURCE_KERNEL;

  sfu_packet_reinit(pkt);
  return pkt;
}

void sfu_packet_pool_free_meta(sfu_packet_pool_t *pp, sfu_packet_t *pkt) {
  pkt->data = NULL;
  sfu_pool_free(&pp->meta, pkt->pool_index);
}
