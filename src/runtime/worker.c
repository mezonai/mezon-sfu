#include "runtime/worker.h"
#include "runtime/cpu.h"
#include "runtime/signal.h"
#include "net/zerocopy.h"
#include "memory/refcount.h"
#include "util/log.h"

#include <string.h>
#include <unistd.h>

#define SFU_WORKER_SEND_SQ_ENTRIES 1024
#define SFU_WORKER_SEND_CQ_ENTRIES 2048
#define SFU_WORKER_REAP_BATCH      128
#define SFU_WORKER_IDLE_SLEEP_US   200

int sfu_worker_init(sfu_worker_t *w, int core_id, int fd, sfu_packet_pool_t *pp,
                     uint32_t inbox_capacity, int send_bgid) {
    memset(w, 0, sizeof(*w));
    w->core_id = core_id;
    w->fd      = fd;
    w->pp      = pp;

    if (sfu_spsc_ring_init(&w->inbox, inbox_capacity) != 0) {
        SFU_LOG_ERROR("worker %d: failed to init inbox ring", core_id);
        return -1;
    }

    /* Send-only ring: no provided buffers, this worker never recvs. */
    if (sfu_ring_init(&w->send_ring, fd, SFU_WORKER_SEND_SQ_ENTRIES,
                       SFU_WORKER_SEND_CQ_ENTRIES, 0, 0, send_bgid, false) != 0) {
        SFU_LOG_ERROR("worker %d: failed to init send ring", core_id);
        sfu_spsc_ring_destroy(&w->inbox);
        return -1;
    }

    return 0;
}

void sfu_worker_destroy(sfu_worker_t *w) {
    sfu_ring_destroy(&w->send_ring);
    sfu_spsc_ring_destroy(&w->inbox);
}

/*
 * Placeholder forward step: echoes the packet back to its sender via
 * zero-copy send. This exercises the full refcount / ZC-completion path
 * end to end (pop -> queue send -> reap NOTIF -> release) before any
 * real RTP routing exists. Once rtp/router.c and room/publisher.c land,
 * this is where subscriber fan-out (sfu_fanout_send_zc against the
 * room's actual subscriber list) replaces the echo.
 */
static void worker_forward(sfu_worker_t *w, sfu_packet_t *pkt) {
    struct sockaddr_storage dsts[1];
    socklen_t dst_lens[1];

    memcpy(&dsts[0], &pkt->peer_addr, sizeof(dsts[0]));
    dst_lens[0] = pkt->peer_addr_len;

    size_t queued = sfu_fanout_send_zc(&w->send_ring, pkt, dsts, dst_lens, 1);
    if (queued < 1) {
        SFU_LOG_WARN("worker %d: send queue full, flushing and dropping remainder",
                     w->core_id);
    }

    /* Drop the reference this function was handed (the dispatcher's
     * transferred ownership); the in-flight send(s) hold their own
     * reference each via sfu_ring_queue_send_zc's internal retain. */
    sfu_ring_release_packet(&w->send_ring, w->pp, pkt);
}

static void *worker_thread_main(void *arg) {
    sfu_worker_t *w = (sfu_worker_t *)arg;
    sfu_pin_current_thread_to_core(w->core_id);

    SFU_LOG_INFO("worker %d started", w->core_id);

    while (!sfu_shutdown_requested()) {
        bool did_work = false;

        void *item;
        int drained = 0;
        while (drained < SFU_WORKER_REAP_BATCH && sfu_spsc_ring_pop(&w->inbox, &item)) {
            worker_forward(w, (sfu_packet_t *)item);
            drained++;
            did_work = true;
        }

        if (drained > 0) {
            sfu_ring_submit(&w->send_ring);
        }

        unsigned reaped = sfu_ring_reap(&w->send_ring, SFU_WORKER_REAP_BATCH,
                                         w->pp, NULL, NULL, w);
        if (reaped > 0) did_work = true;

        if (!did_work) {
            usleep(SFU_WORKER_IDLE_SLEEP_US);
        }
    }

    SFU_LOG_INFO("worker %d shutting down", w->core_id);
    return NULL;
}

int sfu_worker_start(sfu_worker_t *w) {
    int rc = pthread_create(&w->thread, NULL, worker_thread_main, w);
    if (rc != 0) {
        SFU_LOG_ERROR("worker %d: pthread_create failed: %d", w->core_id, rc);
        return -1;
    }
    return 0;
}

void sfu_worker_join(sfu_worker_t *w) {
    pthread_join(w->thread, NULL);
}
