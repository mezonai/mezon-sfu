#ifndef SFU_NET_IO_URING_H
#define SFU_NET_IO_URING_H

#include <liburing.h>
#include <stdint.h>
#include <stdbool.h>

#include "sfu/packet.h"
#include "memory/packet_pool.h"

/*
 * sfu_ring_t wraps one io_uring instance plus one kernel provided-buffer
 * ring (IORING_OP_PROVIDE_BUFFERS-style, registered via
 * io_uring_setup_buf_ring). One sfu_ring_t belongs to exactly one core:
 * the dispatcher owns one for recv, each worker owns its own for send.
 *
 * Recv side: a single multishot RECVMSG stays armed and the kernel keeps
 * delivering CQEs into it, each pointing at one provided buffer -- no
 * per-packet re-arm syscall. Payload bytes land directly in the buffer
 * ring's memory; sfu_packet_t wraps that memory (SFU_BUF_SOURCE_KERNEL)
 * rather than copying it into the packet pool's own data slab.
 *
 * Send side: IORING_OP_SEND_ZC lets N fan-out sends reference the same
 * refcounted sfu_packet_t without copying. Completion arrives as two
 * CQEs per send -- see sfu_ring_reap()'s doc comment for the protocol.
 */
typedef struct sfu_ring {
    struct io_uring          ring;

    struct io_uring_buf_ring *buf_ring;
    void                     *buf_ring_mem;   /* payload backing storage    */
    uint32_t                  buf_count;      /* power of two               */
    uint32_t                  buf_size;
    int                        bgid;

    /* Persistent recvmsg template. Must stay alive for as long as the
     * multishot recv request is armed -- the kernel references it across
     * every completion the single submission produces. */
    struct msghdr              recv_msg_template;

    int                         fd;           /* UDP socket this ring services */
} sfu_ring_t;

/* CQE user_data tagging: bit0 distinguishes RECV completions (tag=1) from
 * SEND_ZC completions, where user_data is the sfu_packet_t* itself
 * (guaranteed even -- see sfu_ring_init's alignment note). */
#define SFU_CQE_TAG_RECV   0x1ULL
#define SFU_CQE_TAG_MASK   0x1ULL

/* with_recv_bufs=false skips provided-buffer-ring setup entirely, for
 * send-only rings (workers) that never arm a multishot recv. buf_count/
 * buf_size/bgid are ignored in that case. */
int  sfu_ring_init(sfu_ring_t *r, int fd, uint32_t sq_entries, uint32_t cq_entries,
                    uint32_t buf_count, uint32_t buf_size, int bgid,
                    bool with_recv_bufs);
void sfu_ring_destroy(sfu_ring_t *r);

/* Arms (or re-arms, if the kernel ever exhausts the multishot request --
 * e.g. on ENOBUFS when the provided buffer ring runs dry) the recv path.
 * Does not call io_uring_submit(); caller batches that. */
int sfu_ring_arm_recv(sfu_ring_t *r);

/* Queues one send_zc from a refcounted packet. Retains the packet before
 * queuing (the in-flight send is itself a reference); the corresponding
 * release happens in sfu_ring_reap() when the NOTIF completion arrives.
 * Does not submit; caller batches sends and calls sfu_ring_submit(). */
int sfu_ring_queue_send_zc(sfu_ring_t *r, sfu_packet_t *pkt,
                            const struct sockaddr *dst, socklen_t dst_len);

/* Flushes all queued SQEs to the kernel in one syscall. */
int sfu_ring_submit(sfu_ring_t *r);

/*
 * Drains up to max_count ready completions in one batch and dispatches:
 *   - on_recv(user_data, pkt): called once per received datagram. pkt has
 *     buf_source == SFU_BUF_SOURCE_KERNEL and refcount == 1 (this ring's
 *     reference). Ownership passes to the callback -- it must eventually
 *     release the packet (sfu_ring_release_packet) itself, e.g. after
 *     fanning it out to worker queues.
 *   - on_send_complete(user_data, pkt): called when a send_zc's buffer
 *     is safe to release (the IORING_CQE_F_NOTIF completion, *not* the
 *     initial "send submitted" completion -- releasing on the first CQE
 *     is a use-after-free waiting to happen once the NIC is still mid-DMA
 *     on the buffer).
 * Returns the number of CQEs processed.
 */
typedef void (*sfu_on_recv_fn)(void *user_data, sfu_packet_t *pkt);
typedef void (*sfu_on_send_complete_fn)(void *user_data, sfu_packet_t *pkt);

unsigned sfu_ring_reap(sfu_ring_t *r, unsigned max_count,
                        sfu_packet_pool_t *pp,
                        sfu_on_recv_fn on_recv,
                        sfu_on_send_complete_fn on_send_complete,
                        void *user_data);

/* Releases a packet's reference; recycles it (kernel buf_ring_add for
 * SFU_BUF_SOURCE_KERNEL, or packet_pool free for SFU_BUF_SOURCE_POOL)
 * once the refcount reaches zero. Safe to call from any core, but the
 * SFU_BUF_SOURCE_KERNEL recycle path must run on the ring that owns the
 * buffer ring the packet came from (i.e. hand it back to the dispatcher
 * if a worker is the one dropping the last reference). */
void sfu_ring_release_packet(sfu_ring_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt);

#endif /* SFU_NET_IO_URING_H */
