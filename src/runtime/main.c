#include <stdlib.h>
#include <unistd.h>

#include "memory/packet_pool.h"
#include "net/socket.h"
#include "peer/session.h"
#include "protocol/signaling/signaling.h"
#include "room/room.h"
#include "runtime/cpu.h"
#include "runtime/fanout.h"
#include "runtime/scheduler.h"
#include "runtime/signal.h"
#include "runtime/worker.h"
#include "sfu/config.h"
#include "sfu/version.h"
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "transport/stun/stun.h"
#include "util/alloc.h"
#include "util/log.h"

static uint16_t parse_port(int argc, char **argv, int index,
                           uint16_t default_port) {
  if (argc > index) {
    int p = atoi(argv[index]);
    if (p > 0 && p < 65536)
      return (uint16_t)p;
  }
  return default_port;
}

int main(int argc, char **argv) {
  sfu_log_set_level(SFU_LOG_LEVEL_INFO);
  SFU_LOG_INFO("mezon-sfu %s starting", SFU_VERSION_STRING);

  uint16_t port = parse_port(argc, argv, 1, SFU_DEFAULT_MEDIA_PORT);
  uint16_t signaling_port =
      parse_port(argc, argv, 2, SFU_DEFAULT_SIGNALING_PORT);

  int online = sfu_online_cpu_count();
  uint32_t worker_count = (uint32_t)(online > 1 ? online - 1 : 1);
  if (worker_count > SFU_MAX_WORKERS)
    worker_count = SFU_MAX_WORKERS;
  SFU_LOG_INFO("detected %d online cpus: 1 dispatcher + %u workers", online,
               worker_count);

  sfu_install_shutdown_handler();

  if (sfu_srtp_global_init() != 0) {
    SFU_LOG_ERROR("failed to init SRTP library");
    return 1;
  }

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
  if (sfu_packet_pool_init(&pp, SFU_PACKET_POOL_CAPACITY,
                           SFU_PACKET_BUF_SIZE) != 0) {
    SFU_LOG_ERROR("failed to init packet pool");
    close(fd);
    return 1;
  }

  sfu_worker_t *workers = SFU_CALLOC(worker_count, sizeof(sfu_worker_t));
  if (!workers) {
    SFU_LOG_ERROR("failed to allocate worker array");
    return 1;
  }

  sfu_room_t room;
  if (sfu_room_init(&room) != 0) {
    SFU_LOG_ERROR("failed to init room registry");
    return 1;
  }

  sfu_fanout_mesh_t mesh;
  if (sfu_fanout_mesh_init(&mesh, worker_count, SFU_FANOUT_RING_CAPACITY,
                           SFU_FANOUT_JOB_POOL_CAPACITY) != 0) {
    SFU_LOG_ERROR("failed to init fanout mesh");
    return 1;
  }

  sfu_dtls_ctx_t dtls_ctx;
  if (sfu_dtls_ctx_init(&dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS context");
    return 1;
  }

  sfu_session_table_t sessions;
  if (sfu_session_table_init(&sessions, &dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init session table");
    return 1;
  }

  sfu_ice_credentials_t ice_creds;
  sfu_ice_credentials_generate(&ice_creds);
  SFU_LOG_INFO("local ICE credentials: ufrag=%s pwd=%s", ice_creds.ufrag,
               ice_creds.pwd);

  const char *public_host = getenv("SFU_PUBLIC_HOST");
  if (!public_host)
    public_host = "127.0.0.1";

  sfu_signaling_server_t signaling;
  if (sfu_signaling_server_start(&signaling, signaling_port, public_host, port,
                                 &ice_creds, &dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to start signaling server");
    return 1;
  }

  for (uint32_t i = 0; i < worker_count; i++) {
    int core_id = (int)(i + 1) % (online > 1 ? online : 1);
    int send_bgid = SFU_PROVIDED_BUF_GROUP_ID + 1 +
                    (int)i; /* distinct bgid per ring, unused for send-only but
                               kept unique */
    if (sfu_worker_init(&workers[i], core_id, i, fd, &pp, &room, &mesh,
                        &sessions, &ice_creds, SFU_WORKER_QUEUE_CAPACITY,
                        send_bgid) != 0) {
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

  SFU_LOG_INFO(
      "mezon-sfu ready: media UDP port %u, signaling ws://%s:%u (pid=%d)", port,
      public_host, signaling_port, getpid());

  sfu_scheduler_join(&scheduler);
  for (uint32_t i = 0; i < worker_count; i++) {
    sfu_worker_join(&workers[i]);
  }

  sfu_scheduler_destroy(&scheduler);
  for (uint32_t i = 0; i < worker_count; i++) {
    sfu_worker_destroy(&workers[i]);
  }
  SFU_FREE(workers);
  sfu_signaling_server_stop(&signaling);
  sfu_fanout_mesh_destroy(&mesh);
  sfu_room_destroy(&room);
  sfu_session_table_destroy(&sessions);
  sfu_dtls_ctx_destroy(&dtls_ctx);
  sfu_packet_pool_destroy(&pp);
  close(fd);
  sfu_srtp_global_deinit();

  SFU_LOG_INFO("mezon-sfu stopped cleanly");
  return 0;
}
