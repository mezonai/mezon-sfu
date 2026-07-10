#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sfu/config.h"
#include "sfu/version.h"
#include "net/socket.h"
#include "runtime/scheduler.h"
#include "runtime/worker.h"
#include "runtime/signal.h"
#include "runtime/cpu.h"
#include "memory/packet_pool.h"
#include "util/log.h"

/*
 * Topology this wires up (see README/docs for the full diagram):
 *
 *   [NIC] -> [dispatcher core: multishot recvmsg, SSRC/4-tuple hash]
 *                  |  SPSC ring per worker
 *                  v
 *   [worker core 0..N: pop inbox, forward via send_zc, reap completions]
 *
 * Core 0 is reserved for the dispatcher; cores 1..N-1 are workers. This
 * is a placeholder policy -- production topology should account for NUMA
 * nodes and leave a core free for the kernel's network softirq handling,
 * but the mapping itself is what matters for now: one dispatcher, N
 * workers, no shared mutable state between them beyond the SPSC rings.
 */

static uint16_t parse_port(int argc, char **argv) {
    if (argc >= 2) {
        int p = atoi(argv[1]);
        if (p > 0 && p < 65536) return (uint16_t)p;
    }
    return SFU_DEFAULT_MEDIA_PORT;
}

int main(int argc, char **argv) {
    sfu_log_set_level(SFU_LOG_LEVEL_INFO);
    SFU_LOG_INFO("mezon-sfu %s starting", SFU_VERSION_STRING);

    uint16_t port = parse_port(argc, argv);

    int online = sfu_online_cpu_count();
    uint32_t worker_count = (uint32_t)(online > 1 ? online - 1 : 1);
    if (worker_count > SFU_MAX_WORKERS) worker_count = SFU_MAX_WORKERS;
    SFU_LOG_INFO("detected %d online cpus: 1 dispatcher + %u workers",
                 online, worker_count);

    sfu_install_shutdown_handler();

    int fd = sfu_udp_socket_create(port);
    if (fd < 0) {
        return 1;
    }

    sfu_packet_pool_t pp;
    /* Meta slots must comfortably exceed in-flight packets across the
     * dispatcher's recv ring + every worker inbox + every worker's
     * in-flight sends; data slots are unused for recv (kernel-buffer-ring
     * backed) but still needed for any locally-originated packets
     * (RTCP, etc.) once those modules exist. */
    if (sfu_packet_pool_init(&pp, SFU_PACKET_POOL_CAPACITY, SFU_PACKET_BUF_SIZE) != 0) {
        SFU_LOG_ERROR("failed to init packet pool");
        close(fd);
        return 1;
    }

    sfu_worker_t *workers = calloc(worker_count, sizeof(sfu_worker_t));
    if (!workers) {
        SFU_LOG_ERROR("failed to allocate worker array");
        return 1;
    }

    for (uint32_t i = 0; i < worker_count; i++) {
        int core_id = (int)(i + 1) % (online > 1 ? online : 1);
        int send_bgid = SFU_PROVIDED_BUF_GROUP_ID + 1 + (int)i; /* distinct bgid per ring, unused for send-only but kept unique */
        if (sfu_worker_init(&workers[i], core_id, fd, &pp,
                             SFU_WORKER_QUEUE_CAPACITY, send_bgid) != 0) {
            SFU_LOG_ERROR("failed to init worker %u", i);
            return 1;
        }
        if (sfu_worker_start(&workers[i]) != 0) {
            SFU_LOG_ERROR("failed to start worker %u", i);
            return 1;
        }
    }

    sfu_scheduler_t scheduler;
    if (sfu_scheduler_init(&scheduler, 0, fd, &pp, workers, worker_count,
                            SFU_PROVIDED_BUF_GROUP_ID, SFU_PROVIDED_BUF_COUNT,
                            SFU_PACKET_BUF_SIZE) != 0) {
        SFU_LOG_ERROR("failed to init scheduler");
        return 1;
    }
    if (sfu_scheduler_start(&scheduler) != 0) {
        SFU_LOG_ERROR("failed to start scheduler");
        return 1;
    }

    SFU_LOG_INFO("mezon-sfu ready on UDP port %u (pid=%d)", port, getpid());

    sfu_scheduler_join(&scheduler);
    for (uint32_t i = 0; i < worker_count; i++) {
        sfu_worker_join(&workers[i]);
    }

    sfu_scheduler_destroy(&scheduler);
    for (uint32_t i = 0; i < worker_count; i++) {
        sfu_worker_destroy(&workers[i]);
    }
    free(workers);
    sfu_packet_pool_destroy(&pp);
    close(fd);

    SFU_LOG_INFO("mezon-sfu stopped cleanly");
    return 0;
}
