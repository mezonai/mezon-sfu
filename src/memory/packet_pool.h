#ifndef SFU_MEMORY_PACKET_POOL_H
#define SFU_MEMORY_PACKET_POOL_H

#include "memory/pool.h"
#include "sfu/packet.h"

/*
 * Packet pool: pairs two parallel slab pools --
 *   - metadata pool of sfu_packet_t (small, fixed struct size)
 *   - data pool of SFU_PACKET_BUF_SIZE byte buffers
 * so metadata and payload have independent, MTU-sized cache footprints.
 * Both pools are sized to the same capacity and index 1:1, so
 * pkt->pool_index always addresses the matching data buffer too.
 */
typedef struct sfu_packet_pool {
  sfu_pool_t meta; /* slots of sizeof(sfu_packet_t) */
  sfu_pool_t data; /* slots of SFU_PACKET_BUF_SIZE  */
} sfu_packet_pool_t;

int sfu_packet_pool_init(sfu_packet_pool_t *pp, uint32_t capacity,
                         uint32_t buf_size);
void sfu_packet_pool_destroy(sfu_packet_pool_t *pp);

/* Allocates a packet slot; data buffer is uninitialized. Refcount starts
 * at 1 (caller's reference). Returns NULL if the pool is exhausted --
 * callers on the recv hot path must treat this as backpressure, not a
 * fatal error (drop the incoming datagram and bump a metric). */
sfu_packet_t *sfu_packet_pool_alloc(sfu_packet_pool_t *pp);

/* Returns a packet (metadata + data buffer) to the pool. Must only be
 * called once refcount has reached zero via sfu_packet_release(). Only
 * valid for packets with buf_source == SFU_BUF_SOURCE_POOL. */
void sfu_packet_pool_free(sfu_packet_pool_t *pp, sfu_packet_t *pkt);

/* Allocates metadata only (no data buffer) -- used when the payload lives
 * in a kernel provided-buffer-ring slot instead of this pool's data slab.
 * Caller (net/io_uring.c) fills in ->data/->cap/->kbuf_index/->buf_source
 * itself. Returns NULL if the metadata pool is exhausted. */
sfu_packet_t *sfu_packet_pool_alloc_meta(sfu_packet_pool_t *pp);

/* Releases metadata only; the caller is responsible for having already
 * returned the kernel buffer slot (io_uring_buf_ring_add) before calling
 * this. Only valid for buf_source == SFU_BUF_SOURCE_KERNEL. */
void sfu_packet_pool_free_meta(sfu_packet_pool_t *pp, sfu_packet_t *pkt);

#endif /* SFU_MEMORY_PACKET_POOL_H */
