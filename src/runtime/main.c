#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

typedef enum {
  SFU_RUN_ALL = 0,
  SFU_RUN_SIGNALING_ONLY,
  SFU_RUN_MEDIA_ONLY,
} sfu_run_mode_t;

static void print_usage(const char *argv0) {
  fprintf(
      stderr,
      "usage: %s [--signaling-only | --media-only] [media_port] "
      "[signaling_port]\n"
      "\n"
      "  --signaling-only   run just the WebSocket signaling server (no UDP\n"
      "                     media socket, no dispatcher/workers). Still\n"
      "                     generates a DTLS cert and ICE credentials so it\n"
      "                     can hand out valid SDP answers.\n"
      "  --media-only       run just the UDP media path (dispatcher, workers,\n"
      "                     STUN/DTLS/SRTP, room forwarding) with no\n"
      "                     signaling server -- a peer must be told the\n"
      "                     logged ICE ufrag/pwd/fingerprint out of band.\n"
      "  (default)          run both.\n"
      "\n"
      "  media_port         UDP port for RTP/RTCP/STUN/DTLS (default %d)\n"
      "  signaling_port     TCP port for WebSocket signaling (default %d)\n",
      argv0, SFU_DEFAULT_MEDIA_PORT, SFU_DEFAULT_SIGNALING_PORT);
}

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

  sfu_run_mode_t mode = SFU_RUN_ALL;
  int positional[8];
  int positional_count = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--signaling-only") == 0) {
      if (mode != SFU_RUN_ALL) {
        print_usage(argv[0]);
        return 1;
      }
      mode = SFU_RUN_SIGNALING_ONLY;
    } else if (strcmp(argv[i], "--media-only") == 0) {
      if (mode != SFU_RUN_ALL) {
        print_usage(argv[0]);
        return 1;
      }
      mode = SFU_RUN_MEDIA_ONLY;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else if ((size_t)positional_count <
               sizeof(positional) / sizeof(positional[0])) {
      positional[positional_count++] = i;
    }
  }

  bool run_media = (mode != SFU_RUN_SIGNALING_ONLY);
  bool run_signaling = (mode != SFU_RUN_MEDIA_ONLY);

  SFU_LOG_INFO("mezon-sfu %s starting (mode: %s)", SFU_VERSION_STRING,
               mode == SFU_RUN_SIGNALING_ONLY ? "signaling-only"
               : mode == SFU_RUN_MEDIA_ONLY   ? "media-only"
                                              : "media+signaling");

  /* Positional args keep their original meaning regardless of which
   * flags preceded them: first positional = media_port, second =
   * signaling_port. */
  uint16_t port = (positional_count > 0) ? parse_port(argc, argv, positional[0],
                                                      SFU_DEFAULT_MEDIA_PORT)
                                         : SFU_DEFAULT_MEDIA_PORT;
  uint16_t signaling_port =
      (positional_count > 1)
          ? parse_port(argc, argv, positional[1], SFU_DEFAULT_SIGNALING_PORT)
          : SFU_DEFAULT_SIGNALING_PORT;

  sfu_install_shutdown_handler();

  if (sfu_srtp_global_init() != 0) {
    SFU_LOG_ERROR("failed to init SRTP library");
    return 1;
  }

  /* DTLS cert + ICE credentials are cheap and socket-independent --
   * generated in every mode so signaling-only can still hand out a
   * valid fingerprint/ufrag/pwd. */
  sfu_dtls_ctx_t dtls_ctx;
  if (sfu_dtls_ctx_init(&dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS context");
    return 1;
  }

  sfu_ice_credentials_t ice_creds;
  sfu_ice_credentials_generate(&ice_creds);
  const char *public_host = getenv("SFU_PUBLIC_HOST");
  if (!public_host)
    public_host = "127.0.0.1";

  SFU_LOG_INFO("local ICE credentials: ufrag=%s pwd=%s public_host=%s",
               ice_creds.ufrag, ice_creds.pwd, public_host);

  int fd = -1;
  sfu_packet_pool_t pp;
  sfu_worker_t *workers = NULL;
  uint32_t worker_count = 0;
  sfu_room_t room;
  sfu_fanout_mesh_t mesh;
  sfu_session_table_t sessions;
  sfu_scheduler_t scheduler;

  if (run_media) {
    fd = sfu_udp_socket_create(port);
    if (fd < 0)
      return 1;

    if (sfu_packet_pool_init(&pp, SFU_PACKET_POOL_CAPACITY,
                             SFU_PACKET_BUF_SIZE) != 0) {
      SFU_LOG_ERROR("failed to init packet pool");
      close(fd);
      return 1;
    }

    int online = sfu_online_cpu_count();
    worker_count = (uint32_t)(online > 1 ? online - 1 : 1);
    if (worker_count > SFU_MAX_WORKERS)
      worker_count = SFU_MAX_WORKERS;
    SFU_LOG_INFO("detected %d online cpus: 1 dispatcher + %u workers", online,
                 worker_count);

    workers = SFU_CALLOC(worker_count, sizeof(sfu_worker_t));
    if (!workers) {
      SFU_LOG_ERROR("failed to allocate worker array");
      return 1;
    }

    if (sfu_room_init(&room, 0, DEFAULT_ROOM_NAME) != 0) {
      SFU_LOG_ERROR("failed to init room registry");
      return 1;
    }

    if (sfu_fanout_mesh_init(&mesh, worker_count, SFU_FANOUT_RING_CAPACITY,
                             SFU_FANOUT_JOB_POOL_CAPACITY) != 0) {
      SFU_LOG_ERROR("failed to init fanout mesh");
      return 1;
    }

    if (sfu_session_table_init(&sessions, &dtls_ctx) != 0) {
      SFU_LOG_ERROR("failed to init session table");
      return 1;
    }

    for (uint32_t i = 0; i < worker_count; i++) {
      int core_id = (int)(i + 1) % (online > 1 ? online : 1);
      int send_bgid = SFU_PROVIDED_BUF_GROUP_ID + 1 + (int)i;
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
  }

  sfu_signaling_server_t signaling;
  if (run_signaling) {
    if (sfu_signaling_server_start(&signaling, signaling_port, public_host,
                                   port, &ice_creds, &dtls_ctx) != 0) {
      SFU_LOG_ERROR("failed to start signaling server");
      return 1;
    }
  }

  if (run_media && run_signaling) {
    SFU_LOG_INFO(
        "mezon-sfu ready: media UDP port %u, signaling ws://%s:%u (pid=%d)",
        port, public_host, signaling_port, getpid());
  } else if (run_media) {
    SFU_LOG_INFO(
        "mezon-sfu ready: media UDP port %u only, no signaling (pid=%d)", port,
        getpid());
  } else {
    SFU_LOG_INFO(
        "mezon-sfu ready: signaling ws://%s:%u only, no media socket (pid=%d)",
        public_host, signaling_port, getpid());
  }

  /* Block until shutdown. The scheduler's own loop already polls
   * sfu_shutdown_requested(), so joining it blocks correctly when
   * media is running; signaling-only has no such thread to join, so
   * poll the flag directly here instead. */
  if (run_media) {
    sfu_scheduler_join(&scheduler);
  } else {
    while (!sfu_shutdown_requested()) {
      usleep(50000);
    }
  }

  if (run_media) {
    for (uint32_t i = 0; i < worker_count; i++) {
      sfu_worker_join(&workers[i]);
    }
    sfu_scheduler_destroy(&scheduler);
    for (uint32_t i = 0; i < worker_count; i++) {
      sfu_worker_destroy(&workers[i]);
    }
    SFU_FREE(workers);
    sfu_fanout_mesh_destroy(&mesh);
    sfu_room_destroy(&room);
    sfu_session_table_destroy(&sessions);
    sfu_packet_pool_destroy(&pp);
    close(fd);
  }
  if (run_signaling) {
    sfu_signaling_server_stop(&signaling);
  }
  sfu_dtls_ctx_destroy(&dtls_ctx);
  sfu_srtp_global_deinit();

  SFU_LOG_INFO("mezon-sfu stopped cleanly");
  return 0;
}
