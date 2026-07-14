#ifndef SFU_RUNTIME_SCHEDULER_H
#define SFU_RUNTIME_SCHEDULER_H

#include <pthread.h>
#include <stdint.h>

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "runtime/worker.h"

/*
 * The dispatcher owns the single UDP socket's recv path: one pinned core
 * runs a multishot recvmsg loop and routes each arriving packet to a
 * worker's inbox. It never sends anything itself and never touches a
 * worker's send ring or state.
 *
 * Routing key: today this hashes the sender's 4-tuple, which is a
 * reasonable placeholder before real SSRC-based demux exists (rtp/
 * parser.c + room/publisher.c). What matters architecturally is already
 * in place: a fixed mapping from routing key to worker index keeps every
 * packet for the same logical flow on the same core, so per-flow state
 * (jitter buffers, sequence tracking, room membership) an be touched
 * without cross-core locking once those modules land.
 */
typedef struct sfu_scheduler {
  int core_id;
  sfu_ring_t recv_ring;
  sfu_packet_pool_t *pp; /* shared with workers, not owned */

  sfu_worker_t *workers; /* not owned; caller-provided array */
  uint32_t worker_count;

  pthread_t thread;
  int fd;
} sfu_scheduler_t;

int sfu_scheduler_init(sfu_scheduler_t *s, int core_id, int fd,
                       sfu_packet_pool_t *pp, sfu_worker_t *workers,
                       uint32_t worker_count, int recv_bgid, uint32_t buf_count,
                       uint32_t buf_size);
void sfu_scheduler_destroy(sfu_scheduler_t *s);

int sfu_scheduler_start(sfu_scheduler_t *s);
void sfu_scheduler_join(sfu_scheduler_t *s);

#endif /* SFU_RUNTIME_SCHEDULER_H */
