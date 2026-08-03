#ifndef SFU_RUNTIME_FANOUT_H
#define SFU_RUNTIME_FANOUT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "memory/pool.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"
#include "util/ringbuffer.h"

/*
 * A publisher's packet often needs to reach subscribers whose send path
 * lives on a *different* worker core than the one that received it --
 * peers are spread across cores for load balancing, not grouped by
 * room. This mesh is the cross-thread handoff for that case, analogous
 * to mezon-proto-server's handle_cross_thread_inbox for channel
 * broadcast, but built from primitives already proven out here (the
 * lock-free slab pool and the SPSC ring) instead of new machinery.
 *
 * Shape: one SPSC ring per (source worker, dest worker) ordered pair,
 * flattened into a worker_count x worker_count array. Worker i is the
 * sole producer for row i (rings[i][*]) and the sole consumer for
 * column i (rings[*][i]) -- exactly the SPSC contract, just applied
 * worker_count^2 times instead of once. At SFU_MAX_WORKERS=16 that's at
 * most 256 rings; fine.
 *
 * Ring items are sfu_fanout_job_t* allocated from a shared pool rather
 * than raw sfu_packet_t*, because each cross-worker hop needs its own
 * destination address alongside the (shared, refcounted) packet -- N
 * subscribers behind the same remote worker each need a distinct dst,
 * not just the one packet pointer.
 *
 * Refcount contract: sfu_fanout_mesh_enqueue() takes ownership of
 * exactly one reference on `pkt` that the caller must already hold
 * (retain before calling, same discipline as sfu_fanout_send_zc). The
 * consuming worker (sfu_fanout_mesh_drain) queues its own local send_zc
 * (which retains its own separate reference for the in-flight send) and
 * then releases the reference the job carried.
 */
/* Forwarding job kinds (CC-10). READY jobs carry a fully rewritten,
 * SRTP-protected packet (the pre-CC-10 behavior, kept for any non-media
 * users). FORWARD jobs carry an UNPROTECTED plaintext copy of the publisher
 * packet plus immutable routing metadata; the destination (subscriber's)
 * worker performs every per-subscriber mutation — payload-type mapping, RTX
 * cache insert, TWCC extension write + history, SRTP protect — so all egress
 * state for a subscriber is owned by exactly one thread. */
typedef enum sfu_fanout_job_kind {
  SFU_FANOUT_JOB_READY = 0,
  SFU_FANOUT_JOB_FORWARD = 1,
} sfu_fanout_job_kind_t;

typedef struct sfu_fanout_job {
  sfu_packet_t *pkt;
  struct sockaddr_storage dst;
  socklen_t dst_len;
  /* FORWARD jobs only: destination subscriber session, pinned (+1 ref) by the
   * producer; the consumer releases it after processing. NULL for READY. */
  sfu_peer_session_t *subscriber;
  /* FORWARD jobs only: value copy of the publisher's routing metadata for
   * this destination (from the immutable receiver snapshot). */
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  bool has_video;
  uint8_t kind; /* sfu_fanout_job_kind_t */
} sfu_fanout_job_t;

typedef struct sfu_fanout_mesh {
  sfu_pool_t job_pool;    /* shared, thread-safe alloc/free of jobs */
  sfu_spsc_ring_t *rings; /* flattened worker_count x worker_count  */
  uint32_t worker_count;
} sfu_fanout_mesh_t;

int sfu_fanout_mesh_init(sfu_fanout_mesh_t *mesh, uint32_t worker_count, uint32_t ring_capacity, uint32_t job_pool_capacity);
void sfu_fanout_mesh_destroy(sfu_fanout_mesh_t *mesh);

/* Producer side (any worker). Takes ownership of one reference on pkt.
 * Returns false on backpressure (job pool exhausted or the target ring
 * is full) -- caller must release its own reference on failure, since
 * ownership did not transfer. */
bool sfu_fanout_mesh_enqueue(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt, const struct sockaddr_storage *dst_addr,
                             socklen_t dst_len);

/* Producer side for FORWARD jobs (CC-10). Same ownership contract as
 * sfu_fanout_mesh_enqueue for pkt; additionally the caller must have pinned
 * `subscriber` (+1 ref) — ownership of that pin transfers to the job on
 * success and stays with the caller on failure. The video_* fields are value
 * copies of the publisher's routing metadata for this destination. */
bool sfu_fanout_mesh_enqueue_forward(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt,
                                     sfu_peer_session_t *subscriber, const struct sockaddr_storage *dst_addr, socklen_t dst_len, uint32_t video_ssrc,
                                     uint32_t video_rtx_ssrc, uint8_t video_pt, uint8_t video_rtx_pt, bool has_video);

/* Consumer side: pops up to max_count jobs addressed to dst_worker
 * across every source worker's ring into it, invoking on_job for each.
 * Returns the number of jobs drained. */
typedef void (*sfu_fanout_job_fn)(void *user_data, sfu_fanout_job_t *job);

unsigned sfu_fanout_mesh_drain(sfu_fanout_mesh_t *mesh, uint32_t dst_worker, unsigned max_count, sfu_fanout_job_fn on_job, void *user_data);

/* Returns a drained job's struct back to the shared pool. Caller must
 * have already handled job->pkt's reference (queued a send and released
 * its own copy) before calling this. */
void sfu_fanout_mesh_free_job(sfu_fanout_mesh_t *mesh, sfu_fanout_job_t *job);

#endif /* SFU_RUNTIME_FANOUT_H */
