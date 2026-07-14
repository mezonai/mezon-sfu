#ifndef SFU_NET_IO_URING_H
#define SFU_NET_IO_URING_H

#include <liburing.h>
#include <stdbool.h>
#include <stdint.h>

#include "memory/packet_pool.h"
#include "sfu/packet.h"
#include "util/ringbuffer.h"

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
  struct io_uring ring;

  struct io_uring_buf_ring *buf_ring;
  void *buf_ring_mem; /* payload backing storage    */
  uint32_t buf_count; /* power of two               */
  uint32_t buf_size;
  int bgid;

  /* Persistent recvmsg template. Must stay alive for as long as the
   * multishot recv request is armed -- the kernel references it across
   * every completion the single submission produces. */
  struct msghdr recv_msg_template;

  int fd; /* UDP socket this ring services */
} sfu_ring_t;

/* CQE user_data tagging: bit0 distinguishes RECV completions (tag=1) from
 * SEND_ZC completions, where user_data is the sfu_packet_t* itself
 * (guaranteed even -- see sfu_ring_init's alignment note). */
#define SFU_CQE_TAG_RECV 0x1ULL
#define SFU_CQE_TAG_MASK 0x1ULL

/* with_recv_bufs=false skips provided-buffer-ring setup entirely, for
 * send-only rings (workers) that never arm a multishot recv. buf_count/
 * buf_size/bgid are ignored in that case. */
int sfu_ring_init(sfu_ring_t *r, int fd, uint32_t sq_entries,
                  uint32_t cq_entries, uint32_t buf_count, uint32_t buf_size,
                  int bgid, bool with_recv_bufs);
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
 *
 * `release_to_dispatcher`: pass NULL when `r` is the dispatcher's own
 * recv ring (it owns the buf_ring directly, so the NOTIF-completion
 * release path can recycle kernel buffers itself). Pass a worker's own
 * release-queue ring otherwise -- required whenever `r` is a send-only
 * worker ring, since it has no buf_ring of its own and must hand kernel
 * buffer indices back to the dispatcher instead of touching one.
 *
 * Returns the number of CQEs processed.
 */
typedef void (*sfu_on_recv_fn)(void *user_data, sfu_packet_t *pkt);
typedef void (*sfu_on_send_complete_fn)(void *user_data, sfu_packet_t *pkt);

unsigned sfu_ring_reap(sfu_ring_t *r, unsigned max_count, sfu_packet_pool_t *pp,
                       sfu_spsc_ring_t *release_to_dispatcher,
                       sfu_on_recv_fn on_recv,
                       sfu_on_send_complete_fn on_send_complete,
                       void *user_data);

/* Releases a packet's reference; recycles it (kernel buf_ring_add for
 * SFU_BUF_SOURCE_KERNEL, or packet_pool free for SFU_BUF_SOURCE_POOL)
 * once the refcount reaches zero. Only safe to call from the ring `r`
 * that actually owns the buffer ring the packet's kernel buffer (if
 * any) came from -- i.e. the dispatcher, for kernel-sourced packets.
 * Workers must use sfu_worker_release_packet() instead; see its
 * comment for why. */
void sfu_ring_release_packet(sfu_ring_t *r, sfu_packet_pool_t *pp,
                             sfu_packet_t *pkt);

/*
 * Worker-safe packet release. A worker is very often the one dropping
 * the *last* reference to a packet whose kernel buffer lives in the
 * *dispatcher's* provided-buffer ring (e.g. after a remote fan-out
 * send_zc completes) -- but io_uring_buf_ring_add/advance are not
 * thread-safe for concurrent producers, so only the dispatcher's own
 * thread may ever touch its buf_ring. Calling sfu_ring_release_packet()
 * from a worker on a kernel-sourced packet would corrupt the buffer
 * ring exactly the way handle_cross_thread_inbox's missing-else UAF
 * corrupted mimalloc slice metadata in mezon-proto-server.
 *
 * Instead: the packet's *metadata* slot is freed locally (the slab pool
 * is already MPMC-safe), and the *kernel buffer index* is pushed onto
 * an SPSC ring back to the dispatcher, which drains it once per tick
 * via sfu_ring_drain_kernel_buffer_returns() and is the only thread
 * that ever calls io_uring_buf_ring_add for that buffer ring.
 *
 * `to_dispatcher` is the calling worker's dedicated release ring (see
 * runtime/worker.h) -- one per worker, so this stays a proper SPSC
 * producer/consumer pair, no locking needed on either side.
 */
void sfu_worker_release_packet(sfu_packet_pool_t *pp,
                               sfu_spsc_ring_t *to_dispatcher,
                               sfu_packet_t *pkt);

/* Dispatcher-only: drains a worker's kernel-buffer-return ring and
 * hands each index back to the kernel provided-buffer ring in one
 * batched add+advance. Called once per dispatcher tick, once per
 * worker's release ring. */
unsigned sfu_ring_drain_kernel_buffer_returns(sfu_ring_t *r,
                                              sfu_spsc_ring_t *from_worker,
                                              unsigned max_count);

#endif /* SFU_NET_IO_URING_H */
