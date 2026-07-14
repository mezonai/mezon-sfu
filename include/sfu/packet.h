#ifndef SFU_PACKET_H
#define SFU_PACKET_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/*
 * sfu_packet_t is the single buffer type that flows from the NIC all the
 * way to N subscriber sends. It is refcounted so that fan-out forwarding
 * (one publisher packet -> many subscribers) never copies payload bytes.
 *
 * Lifecycle:
 *   1. Allocated from the packet pool when a recv CQE completes; refcount=1
 *      (dispatcher's reference), generation bumped.
 *   2. Fan-out: refcount += N *before* the dispatcher drops its own ref, so
 *      the count never transiently hits zero mid-fanout.
 *   3. Each subscriber send path drops its ref only after the ZC "buffer
 *      released" completion (the second CQE, IORING_CQE_F_MORE cleared),
 *      never after the initial "send submitted" completion.
 *   4. refcount hits 0 -> buffer returned to the pool and re-armed onto the
 *      kernel provided-buffer ring.
 *
 * generation exists to catch use-after-free: any code holding onto a
 * pointer + a stale generation value knows its reference is dead rather
 * than silently reusing a recycled buffer.
 */
/* Where pkt->data physically lives, and therefore how it must be recycled
 * once the refcount reaches zero. */
typedef enum sfu_buf_source {
  SFU_BUF_SOURCE_POOL = 0,   /* owned by packet_pool's own data slab      */
  SFU_BUF_SOURCE_KERNEL = 1, /* a kernel provided-buffer-ring slot; must  */
                             /* be handed back via io_uring_buf_ring_add */
} sfu_buf_source_t;

typedef struct sfu_packet {
  uint8_t *data; /* points into pool-owned buffer memory */
  uint32_t len;  /* valid bytes in data                  */
  uint32_t cap;  /* buffer capacity (SFU_PACKET_BUF_SIZE) */

  _Atomic uint32_t refcount;
  _Atomic uint32_t generation;

  uint16_t pool_index; /* slot index within the packet pool    */
  uint16_t kbuf_index; /* index in the kernel provided buf ring */
  uint8_t buf_source;  /* sfu_buf_source_t                     */

  struct sockaddr_storage peer_addr;
  socklen_t peer_addr_len;

  uint64_t recv_ts_ns; /* monotonic recv timestamp             */
} sfu_packet_t;

#endif /* SFU_PACKET_H */
