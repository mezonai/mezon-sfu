#include "protocol/signaling/signaling.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "peer/session.h"
#include "protocol/signaling/json_lite.h"
#include "protocol/signaling/sdp.h"
#include "protocol/websocket/ws.h"
#include "room/room_registry.h"
#include "util/alloc.h"
#include "util/log.h"

#define SFU_SIGNALING_RECV_CAP 16384
#define SFU_SIGNALING_SDP_CAP 16384
#define SFU_SIGNALING_JSON_CAP 32768

#define SFU_MAX_IP_ROOM_MAPPINGS 512

typedef struct {
  char ip[64];
  sfu_room_t *room;
} sfu_ip_room_map_entry_t;

typedef struct {
  int fd;
  sfu_signaling_server_t *server;
} conn_ctx_t;

static sfu_ip_room_map_entry_t g_ip_room_maps[SFU_MAX_IP_ROOM_MAPPINGS];
static int g_ip_room_map_count = 0;
static pthread_mutex_t g_ip_room_map_mutex = PTHREAD_MUTEX_INITIALIZER;

void sfu_register_ip_room(const char *ip, sfu_room_t *room) {
  pthread_mutex_lock(&g_ip_room_map_mutex);
  for (int i = 0; i < g_ip_room_map_count; i++) {
    if (strcmp(g_ip_room_maps[i].ip, ip) == 0) {
      g_ip_room_maps[i].room = room;
      pthread_mutex_unlock(&g_ip_room_map_mutex);
      return;
    }
  }
  if (g_ip_room_map_count < SFU_MAX_IP_ROOM_MAPPINGS) {
    strncpy(g_ip_room_maps[g_ip_room_map_count].ip, ip, 63);
    g_ip_room_maps[g_ip_room_map_count].room = room;
    g_ip_room_map_count++;
  }
  pthread_mutex_unlock(&g_ip_room_map_mutex);
}

sfu_room_t *sfu_lookup_ip_room(const char *ip) {
  pthread_mutex_lock(&g_ip_room_map_mutex);
  for (int i = 0; i < g_ip_room_map_count; i++) {
    if (strcmp(g_ip_room_maps[i].ip, ip) == 0) {
      sfu_room_t *r = g_ip_room_maps[i].room;
      pthread_mutex_unlock(&g_ip_room_map_mutex);
      return r;
    }
  }
  pthread_mutex_unlock(&g_ip_room_map_mutex);
  return NULL;
}

static void handle_offer(int fd, sfu_signaling_server_t *s, const char *sdp, int sdp_len) {
  char answer[SFU_SIGNALING_SDP_CAP];
  int answer_len = sfu_sdp_build_answer(sdp, (size_t)sdp_len, s->media_host, s->media_port, s->ice_creds->ufrag, s->ice_creds->pwd, s->dtls_ctx->fingerprint,
                                        answer, sizeof(answer));
  if (answer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build SDP answer, dropping offer");
    return;
  }

  char escaped[SFU_SIGNALING_JSON_CAP];
  int escaped_len = sfu_json_escape(answer, (size_t)answer_len, escaped, sizeof(escaped));
  if (escaped_len < 0) {
    SFU_LOG_WARN("signaling: answer too large to escape into JSON response");
    return;
  }

  char response[SFU_SIGNALING_JSON_CAP + 64];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"answer\",\"sdp\":\"%s\"}", escaped);
  if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
    SFU_LOG_WARN("signaling: response too large to send");
    return;
  }

  if (sfu_ws_send_text(fd, response, (size_t)response_len) != 0) {
    SFU_LOG_WARN("signaling: failed to send answer over WebSocket");
    return;
  }

  SFU_LOG_INFO("signaling: sent SDP answer (%d bytes) for a %d-byte offer", response_len, sdp_len);
}

static void publish_join_event_to_nats(sfu_signaling_server_t *s, uint64_t room_id, const char *peer_ip) {
  SFU_LOG_INFO(
      "INTEGRATION: [NATS Publish] Topic: sfu.room.join | Payload: "
      "{\"room\": %" PRIu64 ", \"ip\": \"%s\"}",
      room_id, peer_ip);
  /*
     Integration Point:
     If using libnats (C client for NATS):
     natsConnection_PublishString(s->nats_conn, "sfu.room.join", json_payload);
  */
}

static void *conn_thread_main(void *arg) {
  conn_ctx_t *ctx = (conn_ctx_t *)arg;
  int fd = ctx->fd;
  sfu_signaling_server_t *s = ctx->server;
  SFU_FREE(ctx);

  if (sfu_ws_handshake(fd) != 0) {
    SFU_LOG_WARN("signaling: WebSocket handshake failed");
    close(fd);
    return NULL;
  }

  // Get client socket address to map signaling to UDP session
  struct sockaddr_storage peer_addr;
  socklen_t peer_addr_len = sizeof(peer_addr);
  if (getpeername(fd, (struct sockaddr *)&peer_addr, &peer_addr_len) != 0) {
    close(fd);
    return NULL;
  }

  // Convert IP to string for logging/NATS payload
  char peer_ip[64] = "unknown";
  if (peer_addr.ss_family == AF_INET) {
    struct sockaddr_in *s4 = (struct sockaddr_in *)&peer_addr;
    inet_ntop(AF_INET, &s4->sin_addr, peer_ip, sizeof(peer_ip));
  } else if (peer_addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&peer_addr;
    inet_ntop(AF_INET6, &s6->sin6_addr, peer_ip, sizeof(peer_ip));
  }

  char buf[SFU_SIGNALING_RECV_CAP];
  for (;;) {
    ssize_t n = sfu_ws_recv_text(fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }

    char type[32];
    if (sfu_json_extract_string(buf, "type", type, sizeof(type)) < 0) {
      continue;
    }

    if (strcmp(type, "join") == 0) {
      char room_str[64] = {0};
      uint64_t room_id = 0;

      if (sfu_json_extract_string(buf, "room", room_str, sizeof(room_str)) >= 0) {
        room_id = (uint64_t)strtoull(room_str, NULL, 10);
      }

      if (room_id == 0) {
        sfu_ws_send_text(fd, "{\"type\":\"error\",\"message\":\"invalid_room\"}", 41);
        continue;
      }

      // Find or create the session in shared memory
      sfu_peer_session_t *session = sfu_session_table_get_or_create(s->sessions, &peer_addr, peer_addr_len);
      if (session) {
        sfu_room_t *room = sfu_room_registry_get_or_create(s->room_registry, room_id);
        if (room) {
          session->room = room;

          // Register the IP-to-Room mapping for the dynamic UDP media session!
          sfu_register_ip_room(peer_ip, room);

          publish_join_event_to_nats(s, room_id, peer_ip);

          char response[128];
          snprintf(response, sizeof(response), "{\"type\":\"joined\",\"room\":\"%" PRIu64 "\"}", room_id);
          sfu_ws_send_text(fd, response, strlen(response));
        } else {
          sfu_ws_send_text(fd, "{\"type\":\"error\",\"message\":\"room_creation_failed\"}", 49);
        }
      } else {
        sfu_ws_send_text(fd, "{\"type\":\"error\",\"message\":\"session_creation_failed\"}", 52);
      }
    } else if (strcmp(type, "offer") == 0) {
      // Find the session (must exist if they called join first)
      sfu_peer_session_t *session = sfu_session_table_find(s->sessions, &peer_addr, peer_addr_len);
      if (!session || !session->room) {
        SFU_LOG_WARN("signaling: offer received before join completed for peer");
        sfu_ws_send_text(fd, "{\"type\":\"error\",\"message\":\"must_join_room_first\"}", 49);
        continue;
      }

      char sdp[SFU_SIGNALING_SDP_CAP];
      int sdp_len = sfu_json_extract_string(buf, "sdp", sdp, sizeof(sdp));
      if (sdp_len >= 0) {
        handle_offer(fd, s, sdp, sdp_len);
      }
    }
  }

  close(fd);
  return NULL;
}

static void *accept_loop_main(void *arg) {
  sfu_signaling_server_t *s = (sfu_signaling_server_t *)arg;

  while (s->running) {
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0) {
      if (s->running) {
        SFU_LOG_WARN("signaling: accept() failed");
      }
      continue; /* if !running, loop condition will exit next check */
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    conn_ctx_t *ctx = SFU_MALLOC(sizeof(conn_ctx_t));
    ctx->fd = fd;
    ctx->server = s;

    pthread_t tid;
    if (pthread_create(&tid, NULL, conn_thread_main, ctx) != 0) {
      SFU_LOG_ERROR("signaling: failed to spawn connection thread");
      close(fd);
      SFU_FREE(ctx);
      continue;
    }
    pthread_detach(tid);
  }

  return NULL;
}

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port, const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds, const sfu_dtls_ctx_t *dtls_ctx, sfu_session_table_t *sessions,
                               sfu_room_registry_t *room_registry) {
  memset(s, 0, sizeof(*s));
  strncpy(s->media_host, media_host, sizeof(s->media_host) - 1);
  s->media_port = media_port;
  s->ice_creds = ice_creds;
  s->dtls_ctx = dtls_ctx;

  // Assign the shared memory state pointers
  s->sessions = sessions;
  s->room_registry = room_registry;

  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->listen_fd < 0) {
    SFU_LOG_ERROR("signaling: socket() failed");
    return -1;
  }

  int one = 1;
  setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(listen_port);

  if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    SFU_LOG_ERROR("signaling: bind() to port %u failed", listen_port);
    close(s->listen_fd);
    return -1;
  }
  if (listen(s->listen_fd, 16) < 0) {
    SFU_LOG_ERROR("signaling: listen() failed");
    close(s->listen_fd);
    return -1;
  }

  s->running = 1;
  if (pthread_create(&s->thread, NULL, accept_loop_main, s) != 0) {
    SFU_LOG_ERROR("signaling: failed to spawn accept loop thread");
    close(s->listen_fd);
    return -1;
  }

  SFU_LOG_INFO("signaling server listening on ws://0.0.0.0:%u", listen_port);
  return 0;
}

void sfu_signaling_server_stop(sfu_signaling_server_t *s) {
  s->running = 0;
  shutdown(s->listen_fd, SHUT_RDWR);
  close(s->listen_fd);
  pthread_join(s->thread, NULL);
}
