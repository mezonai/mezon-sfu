#ifndef SFU_CONFIG_H
#define SFU_CONFIG_H

/*
 * Compile-time tunables for mezon-sfu.
 * Values here favor predictable, cache-friendly sizing over dynamic
 * flexibility -- this is a hot-path media server, not a general service.
 */

/* -------- networking / io_uring -------- */
#define SFU_MAX_WORKERS            16      /* hard cap on worker cores        */
#define SFU_RING_SQ_ENTRIES        4096    /* submission queue depth per ring */
#define SFU_RING_CQ_ENTRIES        8192    /* completion queue depth per ring */

/* MTU-class packet buffer size. 1500 (Ethernet) minus IP/UDP/SRTP headroom,
 * rounded up for alignment. Buffers are never resized at runtime. */
#define SFU_PACKET_BUF_SIZE        1600
#define SFU_PACKET_POOL_CAPACITY   65536   /* per-process packet slots        */

/* Kernel-registered provided buffer ring (io_uring_setup_buf_ring). Must be
 * a power of two. */
#define SFU_PROVIDED_BUF_COUNT     8192
#define SFU_PROVIDED_BUF_GROUP_ID  0

/* Dispatcher -> worker SPSC ring buffer capacity. Must be a power of two. */
#define SFU_WORKER_QUEUE_CAPACITY  16384

/* -------- ports -------- */
#define SFU_DEFAULT_MEDIA_PORT     7000    /* shared RTP/RTCP/STUN UDP port   */

/* -------- misc -------- */
#define SFU_CACHELINE_SIZE         64

#define SFU_LIKELY(x)              __builtin_expect(!!(x), 1)
#define SFU_UNLIKELY(x)            __builtin_expect(!!(x), 0)

#endif /* SFU_CONFIG_H */
