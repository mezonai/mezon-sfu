#include "runtime/scheduler.h"
#include "runtime/cpu.h"
#include "runtime/signal.h"
#include "util/log.h"

#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

#define SFU_DISPATCH_SQ_ENTRIES  1024
#define SFU_DISPATCH_CQ_ENTRIES  4096
#define SFU_DISPATCH_REAP_BATCH  256
#define SFU_DISPATCH_IDLE_SLEEP_US 100

int sfu_scheduler_init(sfu_scheduler_t *s, int core_id, int fd,
                        sfu_packet_pool_t *pp, sfu_worker_t *workers,
                        uint32_t worker_count, int recv_bgid,
                        uint32_t buf_count, uint32_t buf_size) {
    memset(s, 0, sizeof(*s));
    s->core_id      = core_id;
    s->fd           = fd;
    s->pp           = pp;
    s->workers      = workers;
    s->worker_count = worker_count;

    if (sfu_ring_init(&s->recv_ring, fd, SFU_DISPATCH_SQ_ENTRIES,
                       SFU_DISPATCH_CQ_ENTRIES, buf_count, buf_size,
                       recv_bgid, true) != 0) {
        SFU_LOG_ERROR("scheduler: failed to init recv ring");
        return -1;
    }

    return 0;
}

void sfu_scheduler_destroy(sfu_scheduler_t *s) {
    sfu_ring_destroy(&s->recv_ring);
}

/* FNV-1a over the sender's address bytes -- cheap, decent distribution
 * for the placeholder 4-tuple routing key described in scheduler.h. */
static uint32_t hash_peer_addr(const struct sockaddr_storage *addr, socklen_t len) {
    const uint8_t *bytes = (const uint8_t *)addr;
    uint32_t h = 2166136261u;
    for (socklen_t i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

typedef struct {
    sfu_scheduler_t *s;
} recv_ctx_t;

static void on_recv(void *user_data, sfu_packet_t *pkt) {
    sfu_scheduler_t *s = ((recv_ctx_t *)user_data)->s;

    uint32_t h = hash_peer_addr(&pkt->peer_addr, pkt->peer_addr_len);
    uint32_t worker_idx = h % s->worker_count;

    /* Ownership of this reference transfers into the inbox. If the
     * worker's inbox is full (backpressure), we must not leak the
     * packet -- release it here, which recycles the kernel buffer. */
    if (!sfu_spsc_ring_push(&s->workers[worker_idx].inbox, pkt)) {
        SFU_LOG_WARN("worker %u inbox full, dropping packet", worker_idx);
        sfu_ring_release_packet(&s->recv_ring, s->pp, pkt);
    }
}

static void *scheduler_thread_main(void *arg) {
    sfu_scheduler_t *s = (sfu_scheduler_t *)arg;
    sfu_pin_current_thread_to_core(s->core_id);

    recv_ctx_t ctx = { .s = s };

    if (sfu_ring_arm_recv(&s->recv_ring) != 0) {
        SFU_LOG_ERROR("scheduler: failed to arm initial recv");
        return NULL;
    }
    sfu_ring_submit(&s->recv_ring);

    SFU_LOG_INFO("scheduler (dispatcher) started on core %d", s->core_id);

    while (!sfu_shutdown_requested()) {
        unsigned reaped = sfu_ring_reap(&s->recv_ring, SFU_DISPATCH_REAP_BATCH,
                                         s->pp, NULL, on_recv, NULL, &ctx);

        /* Only this thread may touch its own buf_ring -- drain every
         * worker's release queue here and hand kernel buffer indices
         * back to the kernel, batched per worker. See
         * sfu_worker_release_packet's doc for why workers can't do
         * this themselves. */
        unsigned returned = 0;
        for (uint32_t i = 0; i < s->worker_count; i++) {
            returned += sfu_ring_drain_kernel_buffer_returns(
                &s->recv_ring, &s->workers[i].release_to_dispatcher,
                SFU_DISPATCH_REAP_BATCH);
        }

        if (reaped == 0 && returned == 0) {
            usleep(SFU_DISPATCH_IDLE_SLEEP_US);
        }
    }

    SFU_LOG_INFO("scheduler shutting down");
    return NULL;
}

int sfu_scheduler_start(sfu_scheduler_t *s) {
    int rc = pthread_create(&s->thread, NULL, scheduler_thread_main, s);
    if (rc != 0) {
        SFU_LOG_ERROR("scheduler: pthread_create failed: %d", rc);
        return -1;
    }
    return 0;
}

void sfu_scheduler_join(sfu_scheduler_t *s) {
    pthread_join(s->thread, NULL);
}
