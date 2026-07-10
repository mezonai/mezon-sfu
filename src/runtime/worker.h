#ifndef SFU_RUNTIME_WORKER_H
#define SFU_RUNTIME_WORKER_H

#include <pthread.h>
#include <stdint.h>

#include "net/io_uring.h"
#include "memory/packet_pool.h"
#include "util/ringbuffer.h"

/*
 * One sfu_worker_t owns one pinned core: its own io_uring send ring, and
 * an inbox that the dispatcher (scheduler.c) feeds via a lock-free SPSC
 * ring. Workers never touch each other's state and never touch the
 * dispatcher's recv ring -- the only cross-core interaction is the
 * SPSC inbox push (dispatcher side) / pop (worker side).
 *
 * Packet lifecycle across the boundary: the dispatcher retains no
 * reference of its own once a packet is pushed into a worker's inbox --
 * ownership of that single reference transfers with the pointer. The
 * worker is responsible for eventually releasing it via
 * sfu_ring_release_packet(), whether that's after forwarding it,
 * after this skeleton's demonstration echo-send, or after dropping it.
 */
typedef struct sfu_worker {
    int              core_id;
    sfu_ring_t        send_ring;
    sfu_spsc_ring_t   inbox;
    sfu_packet_pool_t *pp;      /* shared with the dispatcher, not owned */

    pthread_t         thread;
    int                fd;       /* same UDP socket the dispatcher recvs on */
} sfu_worker_t;

int  sfu_worker_init(sfu_worker_t *w, int core_id, int fd, sfu_packet_pool_t *pp,
                      uint32_t inbox_capacity, int send_bgid);
void sfu_worker_destroy(sfu_worker_t *w);

/* Spawns the worker's thread, which pins itself to core_id and runs
 * until sfu_shutdown_requested() is observed. */
int  sfu_worker_start(sfu_worker_t *w);
void sfu_worker_join(sfu_worker_t *w);

#endif /* SFU_RUNTIME_WORKER_H */
