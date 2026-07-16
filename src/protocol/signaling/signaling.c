#include "protocol/signaling/signaling.h"
#include "peer/session.h"
#include "protocol/signaling/json_lite.h"
#include "protocol/signaling/sdp.h"
#include "protocol/websocket/ws.h"
#include "room/room_registry.h"
#include "util/alloc.h"
#include "util/log.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SFU_SIGNALING_RECV_CAP 16384
#define SFU_SIGNALING_SDP_CAP 16384
#define SFU_SIGNALING_JSON_CAP 32768

typedef struct {
  int fd;
  sfu_signaling_server_t *server;
} conn_ctx_t;

static void handle_offer(int fd, sfu_signaling_server_t *s, const char *sdp,
                         int sdp_len) {
  char answer[SFU_SIGNALING_SDP_CAP];
  int answer_len = sfu_sdp_build_answer(
      sdp, (size_t)sdp_len, s->media_host, s->media_port, s->ice_creds->ufrag,
      s->ice_creds->pwd, s->dtls_ctx->fingerprint, answer, sizeof(answer));
  if (answer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build SDP answer, dropping offer");
    return;
  }

  char escaped[SFU_SIGNALING_JSON_CAP];
  int escaped_len =
      sfu_json_escape(answer, (size_t)answer_len, escaped, sizeof(escaped));
  if (escaped_len < 0) {
    SFU_LOG_WARN("signaling: answer too large to escape into JSON response");
    return;
  }

  char response[SFU_SIGNALING_JSON_CAP + 64];
  int response_len = snprintf(response, sizeof(response),
                              "{\"type\":\"answer\",\"sdp\":\"%s\"}", escaped);
  if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
    SFU_LOG_WARN("signaling: response too large to send");
    return;
  }

  if (sfu_ws_send_text(fd, response, (size_t)response_len) != 0) {
    SFU_LOG_WARN("signaling: failed to send answer over WebSocket");
    return;
  }

  SFU_LOG_INFO("signaling: sent SDP answer (%d bytes) for a %d-byte offer",
               response_len, sdp_len);
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
  SFU_LOG_INFO("signaling: client connected");

  // Get client socket address to associate signaling connection with the UDP
  // session table
  struct sockaddr_storage peer_addr;
  socklen_t peer_addr_len = sizeof(peer_addr);
  if (getpeername(fd, (struct sockaddr *)&peer_addr, &peer_addr_len) != 0) {
    SFU_LOG_WARN("signaling: failed to get peer name from socket");
    close(fd);
    return NULL;
  }

  char buf[SFU_SIGNALING_RECV_CAP];
  for (;;) {
    ssize_t n = sfu_ws_recv_text(fd, buf, sizeof(buf));
    if (n <= 0)
      break; /* clean close or error */

    char type[32];
    if (sfu_json_extract_string(buf, "type", type, sizeof(type)) < 0) {
      SFU_LOG_WARN("signaling: message with no \"type\" field, ignoring");
      continue;
    }

    if (strcmp(type, "offer") == 0) {
      char sdp[SFU_SIGNALING_SDP_CAP];
      int sdp_len = sfu_json_extract_string(buf, "sdp", sdp, sizeof(sdp));
      if (sdp_len < 0) {
        SFU_LOG_WARN(
            "signaling: offer message with no \"sdp\" field, ignoring");
        continue;
      }

      // EXTRACT room ID from incoming JSON (default to 101 if not present)
      char room_str[64] = {0};
      uint64_t room_id = 101;
      if (sfu_json_extract_string(buf, "room", room_str, sizeof(room_str)) >=
          0) {
        room_id = (uint64_t)strtoull(room_str, NULL, 10);
      }

      // GET OR CREATE the session in the shared table[cite: 8]
      sfu_peer_session_t *session = sfu_session_table_get_or_create(
          s->sessions, &peer_addr, peer_addr_len);
      if (session) {
        // RETRIEVE OR CREATE room and bind it directly to the session in memory
        sfu_room_t *room =
            sfu_room_registry_get_or_create(s->room_registry, room_id);
        if (room) {
          session->room = room;
          SFU_LOG_INFO(
              "signaling: Associated peer session %p with Room ID %" PRIu64,
              (void *)session, room_id);
        } else {
          SFU_LOG_ERROR(
              "signaling: Failed to create or acquire Room ID %" PRIu64,
              room_id);
        }
      } else {
        SFU_LOG_ERROR("signaling: Failed to register session in shared table");
      }

      handle_offer(fd, s, sdp, sdp_len);
    } else {
      SFU_LOG_WARN("signaling: unrecognized message type \"%s\", ignoring",
                   type);
    }
  }

  SFU_LOG_INFO("signaling: client disconnected");
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

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port,
                               const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds,
                               const sfu_dtls_ctx_t *dtls_ctx,
                               sfu_session_table_t *sessions,
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
