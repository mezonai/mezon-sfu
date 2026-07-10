#ifndef SFU_RUNTIME_WORKER_H
#define SFU_RUNTIME_WORKER_H

#include <pthread.h>
#include <stdint.h>

#include "net/io_uring.h"
#include "memory/packet_pool.h"
#include "util/ringbuffer.h"
#include "room/room.h"
#include "runtime/fanout.h"

/*
 * One sfu_worker_t owns one pinned core: its own io_uring send ring, and
 * an inbox that the dispatcher (scheduler.c) feeds via a lock-free SPSC
 * ring. Workers never touch each other's state directly -- the only
 * cross-core interaction is through the SPSC inbox (dispatcher -> this
 * worker), the fan-out mesh (any worker -> any worker, see
 * runtime/fanout.h), and the release queue back to the dispatcher for
 * kernel-buffer recycling (see net/io_uring.h's sfu_worker_release_packet).
 *
 * Packet lifecycle across the dispatcher->worker boundary: the
 * dispatcher retains no reference of its own once a packet is pushed
 * into a worker's inbox -- ownership of that single reference transfers
 * with the pointer. The worker is responsible for eventually releasing
 * it (via sfu_worker_release_packet), whether that's after local
 * send_zc, handing it to the fan-out mesh for a remote subscriber, or
 * dropping it.
 */
typedef struct sfu_worker {
    int              core_id;      /* which CPU core this thread is pinned to */
    uint32_t         worker_index; /* stable 0..worker_count-1 identity, used
                                     * for room/mesh addressing -- distinct
                                     * from core_id so pinning policy can
                                     * change independently of routing */
    sfu_ring_t        send_ring;
    sfu_spsc_ring_t   inbox;             /* dispatcher -> this worker */
    sfu_spsc_ring_t   release_to_dispatcher; /* this worker -> dispatcher, kernel buffer returns */

    sfu_packet_pool_t *pp;      /* shared with the dispatcher, not owned */
    sfu_room_t         *room;    /* shared peer registry, not owned */
    sfu_fanout_mesh_t   *mesh;    /* shared cross-worker fan-out mesh, not owned */

    pthread_t         thread;
    int                fd;       /* same UDP socket the dispatcher recvs on */
} sfu_worker_t;

int  sfu_worker_init(sfu_worker_t *w, int core_id, uint32_t worker_index, int fd,
                      sfu_packet_pool_t *pp, sfu_room_t *room, sfu_fanout_mesh_t *mesh,
                      uint32_t inbox_capacity, int send_bgid);
void sfu_worker_destroy(sfu_worker_t *w);

/* Spawns the worker's thread, which pins itself to core_id and runs
 * until sfu_shutdown_requested() is observed. */
int  sfu_worker_start(sfu_worker_t *w);
void sfu_worker_join(sfu_worker_t *w);

#endif /* SFU_RUNTIME_WORKER_H */
