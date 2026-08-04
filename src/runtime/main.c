#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memory/packet_pool.h"
#include "net/socket.h"
#include "peer/session.h"
#include "protocol/signaling/signaling.h"
#include "room/room_registry.h"
#include "runtime/cpu.h"
#include "runtime/fanout.h"
#include "runtime/routing_context.h"
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

static void print_usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [media_port] [signaling_port]\n"
          "\n"
          "  media_port         UDP port for RTP/RTCP/STUN/DTLS (default %d)\n"
          "  signaling_port     TCP port for WebSocket signaling (default %d)\n",
          argv0, SFU_DEFAULT_MEDIA_PORT, SFU_DEFAULT_SIGNALING_PORT);
}

static uint16_t parse_port(int argc, char **argv, int index, uint16_t default_port) {
  if (argc > index) {
    int p = atoi(argv[index]);
    if (p > 0 && p < 65536) {
      return (uint16_t)p;
    }
  }
  return default_port;
}

int main(int argc, char **argv) {
  _Static_assert(SFU_SESSION_ADDR_HASH_SLOTS >= SFU_SESSION_TABLE_MAX * 2, "addr hash table too small for max load factor target");
  _Static_assert(SFU_SESSION_UFRAG_HASH_SLOTS >= SFU_SESSION_TABLE_MAX * 2, "ufrag hash table too small for max load factor target");
  _Static_assert((SFU_SESSION_ADDR_HASH_SLOTS & (SFU_SESSION_ADDR_HASH_SLOTS - 1)) == 0, "addr hash table size must be power of 2");
  _Static_assert((SFU_SESSION_UFRAG_HASH_SLOTS & (SFU_SESSION_UFRAG_HASH_SLOTS - 1)) == 0, "ufrag hash table size must be power of 2");

  sfu_log_set_level(SFU_LOG_LEVEL_DEBUG);

  signal(SIGPIPE, SIG_IGN);

  int positional[8];
  int positional_count = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if ((size_t)positional_count < sizeof(positional) / sizeof(positional[0])) {
      positional[positional_count++] = i;
    }
  }

  SFU_LOG_INFO("mezon-sfu %s starting (unified signaling & media configuration)", SFU_VERSION_STRING);

  uint16_t port = (positional_count > 0) ? parse_port(argc, argv, positional[0], SFU_DEFAULT_MEDIA_PORT) : SFU_DEFAULT_MEDIA_PORT;
  uint16_t signaling_port = (positional_count > 1) ? parse_port(argc, argv, positional[1], SFU_DEFAULT_SIGNALING_PORT) : SFU_DEFAULT_SIGNALING_PORT;

  sfu_install_shutdown_handler();

  int rc = 1;
  int fd = -1;
  int online = 1;
  uint32_t worker_count = 0, workers_initialized = 0, workers_started = 0;
  bool srtp_initialized = false, dtls_initialized = false;
  bool packet_pool_initialized = false, routing_initialized = false;
  bool room_registry_initialized = false, mesh_initialized = false;
  bool sessions_initialized = false, scheduler_initialized = false;
  bool scheduler_started = false, signaling_started = false;
  sfu_worker_t *workers = NULL;
  sfu_scheduler_t *scheduler = NULL;
  sfu_room_registry_t *room_registry = SFU_CALLOC(1, sizeof(*room_registry));
  sfu_session_table_t *sessions = SFU_CALLOC(1, sizeof(*sessions));
  sfu_routing_table_t *routing_table = SFU_CALLOC(1, sizeof(*routing_table));
  sfu_fanout_mesh_t *mesh = SFU_CALLOC(1, sizeof(*mesh));
  sfu_packet_pool_t *pp = SFU_CALLOC(1, sizeof(*pp));
  sfu_dtls_ctx_t dtls_ctx;
  sfu_signaling_server_t signaling;
  sfu_ice_credentials_t ice_creds;

  if (!room_registry || !sessions || !routing_table || !mesh || !pp) {
    SFU_LOG_ERROR("failed to allocate top-level runtime structures");
    goto cleanup;
  }
  if (sfu_srtp_global_init() != 0) {
    SFU_LOG_ERROR("failed to init SRTP library");
    goto cleanup;
  }
  srtp_initialized = true;
  if (sfu_dtls_ctx_init(&dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS context");
    goto cleanup;
  }
  dtls_initialized = true;
  sfu_ice_credentials_generate(&ice_creds);
  const char *public_host = getenv("SFU_PUBLIC_HOST");
  if (!public_host) {
    public_host = "127.0.0.1";
  }
  SFU_LOG_INFO("local ICE credentials: ufrag=%s pwd=%s public_host=%s", ice_creds.ufrag, ice_creds.pwd, public_host);

  fd = sfu_udp_socket_create(port);
  if (fd < 0) {
    goto cleanup;
  }
  if (sfu_packet_pool_init(pp, SFU_PACKET_POOL_CAPACITY, SFU_PACKET_BUF_SIZE) != 0) {
    SFU_LOG_ERROR("failed to init packet pool");
    goto cleanup;
  }
  packet_pool_initialized = true;
  online = sfu_online_cpu_count();
  worker_count = (uint32_t)(online > 1 ? online - 1 : 1);
  if (worker_count > SFU_MAX_WORKERS) {
    worker_count = SFU_MAX_WORKERS;
  }
  /* Reject impossible worker counts before any io_uring/fanout/scheduler init.
   * Auto-detect always yields >= 1 and is clamped to SFU_MAX_WORKERS above;
   * this guard covers future config overrides and defensive invariants. */
  if (worker_count < 1 || worker_count > SFU_MAX_WORKERS) {
    SFU_LOG_ERROR("invalid worker_count %u (must be 1..%d)", worker_count, SFU_MAX_WORKERS);
    goto cleanup;
  }
  SFU_LOG_INFO("detected %d online cpus: 1 dispatcher + %u workers", online, worker_count);
  workers = SFU_CALLOC(worker_count, sizeof(*workers));
  scheduler = SFU_CALLOC(1, sizeof(*scheduler));
  if (!workers || !scheduler) {
    SFU_LOG_ERROR("failed to allocate runtime threads");
    goto cleanup;
  }
  if (sfu_routing_table_init(routing_table) != 0) {
    goto cleanup;
  }
  routing_initialized = true;
  if (sfu_room_registry_init(room_registry) != 0) {
    goto cleanup;
  }
  room_registry_initialized = true;
  if (sfu_fanout_mesh_init(mesh, worker_count, SFU_FANOUT_RING_CAPACITY, SFU_FANOUT_JOB_POOL_CAPACITY) != 0) {
    goto cleanup;
  }
  mesh_initialized = true;
  if (sfu_session_table_init(sessions, &dtls_ctx) != 0) {
    goto cleanup;
  }
  sessions_initialized = true;
  if (sfu_scheduler_init(scheduler, 0, fd, pp, workers, worker_count, SFU_PROVIDED_BUF_GROUP_ID, SFU_PROVIDED_BUF_COUNT, SFU_PACKET_BUF_SIZE) != 0) {
    goto cleanup;
  }
  scheduler_initialized = true;

  for (uint32_t i = 0; i < worker_count; i++) {
    int core_id = (int)(i + 1) % (online > 1 ? online : 1);
    int send_bgid = SFU_PROVIDED_BUF_GROUP_ID + 1 + (int)i;
    if (sfu_worker_init(&workers[i], core_id, i, fd, pp, room_registry, mesh, sessions, routing_table, &ice_creds, scheduler, SFU_WORKER_QUEUE_CAPACITY,
                        send_bgid) != 0) {
      goto cleanup;
    }
    workers_initialized++;
  }
  /* Start consumers before the dispatcher can enqueue packets for them. */
  for (uint32_t i = 0; i < worker_count; i++) {
    if (sfu_worker_start(&workers[i]) != 0) {
      goto cleanup;
    }
    workers_started++;
  }
  if (sfu_scheduler_start(scheduler) != 0) {
    goto cleanup;
  }
  scheduler_started = true;
  if (sfu_signaling_server_start(&signaling, signaling_port, public_host, port, &ice_creds, &dtls_ctx, sessions, room_registry, routing_table) != 0) {
    goto cleanup;
  }
  signaling_started = true;

  SFU_LOG_INFO("mezon-sfu ready: media UDP port %u, signaling ws://%s:%u (pid=%d)", port, public_host, signaling_port, getpid());
  sfu_scheduler_join(scheduler);
  scheduler_started = false;
  rc = 0;

cleanup:
  if (signaling_started) {
    sfu_signaling_server_stop(&signaling);
  }
  if (rc != 0 && (scheduler_started || workers_started)) {
    sfu_request_shutdown();
  }
  if (scheduler_started) {
    sfu_scheduler_join(scheduler);
  }
  for (uint32_t i = 0; i < workers_started; i++) {
    sfu_worker_join(&workers[i]);
  }
  if (scheduler_initialized) {
    sfu_scheduler_destroy(scheduler);
  }
  for (uint32_t i = 0; i < workers_initialized; i++) {
    sfu_worker_destroy(&workers[i]);
  }
  if (sessions_initialized) {
    sfu_session_table_destroy(sessions);
  }
  if (mesh_initialized) {
    sfu_fanout_mesh_destroy(mesh);
  }
  if (room_registry_initialized) {
    sfu_room_registry_destroy(room_registry);
  }
  if (routing_initialized) {
    sfu_routing_table_destroy(routing_table);
  }
  if (packet_pool_initialized) {
    sfu_packet_pool_destroy(pp);
  }
  if (fd >= 0) {
    close(fd);
  }
  if (dtls_initialized) {
    sfu_dtls_ctx_destroy(&dtls_ctx);
  }
  if (srtp_initialized) {
    sfu_srtp_global_deinit();
  }
  SFU_FREE(scheduler);
  SFU_FREE(workers);
  SFU_FREE(pp);
  SFU_FREE(mesh);
  SFU_FREE(routing_table);
  SFU_FREE(sessions);
  SFU_FREE(room_registry);
  if (rc != 0) {
    return rc;
  }

  SFU_LOG_INFO("mezon-sfu stopped cleanly");
  return 0;
}
