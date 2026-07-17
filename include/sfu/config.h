#ifndef SFU_CONFIG_H
#define SFU_CONFIG_H

#define SFU_MAX_WORKERS 16       /* hard cap on worker cores        */
#define SFU_RING_SQ_ENTRIES 4096 /* submission queue depth per ring */
#define SFU_RING_CQ_ENTRIES 8192 /* completion queue depth per ring */

/* MTU-class packet buffer size. 1500 (Ethernet) minus IP/UDP/SRTP headroom,
 * rounded up for alignment. Buffers are never resized at runtime. */
#define SFU_PACKET_BUF_SIZE 1600
#define SFU_PACKET_POOL_CAPACITY 65536 /* per-process packet slots        */

/* Kernel-registered provided buffer ring (io_uring_setup_buf_ring). Must be
 * a power of two. */
#define SFU_PROVIDED_BUF_COUNT 8192
#define SFU_PROVIDED_BUF_GROUP_ID 0

/* Dispatcher -> worker SPSC ring buffer capacity. Must be a power of two. */
#define SFU_WORKER_QUEUE_CAPACITY 16384

/* One SPSC ring per (source worker, dest worker) pair, so any worker can
 * hand a packet to any other worker's send path without locking. Must be
 * a power of two. */
#define SFU_FANOUT_RING_CAPACITY 4096
/* Shared pool of small "fanout job" structs (packet ptr + destination
 * addr) that ride through the mesh rings -- see runtime/fanout.h. */
#define SFU_FANOUT_JOB_POOL_CAPACITY 16384
/* Per-worker SPSC ring returning kernel provided-buffer indices back to
 * the dispatcher -- only the dispatcher's thread may touch its own
 * io_uring_buf_ring, so a worker that drops the last reference to a
 * kernel-sourced packet must hand the index back rather than recycle it
 * itself. See net/io_uring.c's sfu_worker_release_packet(). */
#define SFU_RELEASE_QUEUE_CAPACITY 8192

/* Fixed-capacity, mutex-guarded peer table. This is a control-plane-style
 * placeholder, NOT the final concurrency design -- every packet currently
 * pays a mutex lock to look up subscribers. Once real join/publish
 * signaling and SSRC-based routing exist, replace with a sharded or
 * RCU/seqlock-style per-room structure so the packet hot path never
 * blocks on a lock (see the mutex-guarded hash table lesson from
 * mezon-proto-server's handshake rate limiter). */
#define SFU_ROOM_MAX_PEERS 256

#define SFU_DEFAULT_MEDIA_PORT 7000     /* shared RTP/RTCP/STUN UDP port   */
#define SFU_DEFAULT_SIGNALING_PORT 8000 /* WebSocket signaling TCP port    */

#define SFU_CACHELINE_SIZE 64

#define DEFAULT_ROOM_NAME "Default Room"

#define SFU_LIKELY(x) __builtin_expect(!!(x), 1)
#define SFU_UNLIKELY(x) __builtin_expect(!!(x), 0)

#endif /* SFU_CONFIG_H */
