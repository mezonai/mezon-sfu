#include "protocol/signaling/signaling.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>
#include "peer/session.h"
#include "protocol/signaling/json_lite.h"
#include "protocol/signaling/sdp.h"
#include "protocol/websocket/ws.h"
#include "room/room_registry.h"
#include "runtime/routing_context.h"
#include "util/alloc.h"
#include "util/log.h"

#define SFU_SIGNALING_RECV_CAP 16384
#define SFU_SIGNALING_SDP_CAP 16384
#define SFU_SIGNALING_JSON_CAP 32768

void sfu_register_ufrag_room(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room) {
  pthread_mutex_lock(&table->mutex);

  for (int i = 0; i < table->count; i++) {
    if (strcmp(table->entries[i].ufrag, client_ufrag) == 0) {
      table->entries[i].room = room;
      pthread_mutex_unlock(&table->mutex);
      return;
    }
  }

  if (table->count < SFU_MAX_UFRAG_MAPPINGS) {
    sfu_routing_entry_t *entry = &table->entries[table->count];
    strncpy(entry->ufrag, client_ufrag, sizeof(entry->ufrag) - 1);
    entry->ufrag[sizeof(entry->ufrag) - 1] = '\0';
    entry->room = room;

    /* Explicitly reset worker ownership so the first worker to see STUN claims it */
    entry->has_owner = false;
    entry->worker_index = 0;

    table->count++;
  } else {
    SFU_LOG_ERROR("ufrag->room table FULL. Cannot register ufrag=%s", client_ufrag);
  }

  pthread_mutex_unlock(&table->mutex);
}

static bool extract_sdp_ice_ufrag(const char *sdp, size_t sdp_len, char *out, size_t out_cap) {
  static const char needle[] = "a=ice-ufrag:";
  const size_t needle_len = sizeof(needle) - 1;

  for (size_t i = 0; i + needle_len <= sdp_len; i++) {
    if (memcmp(sdp + i, needle, needle_len) == 0) {
      size_t start = i + needle_len;
      size_t end = start;
      while (end < sdp_len && sdp[end] != '\r' && sdp[end] != '\n') {
        end++;
      }
      size_t len = end - start;
      if (len == 0 || len >= out_cap) {
        return false;
      }
      memcpy(out, sdp + start, len);
      out[len] = '\0';
      return true;
    }
  }
  return false;
}

typedef struct {
  int fd;
  sfu_signaling_server_t *server;
} conn_ctx_t;

static void extract_sdp_ssrcs(const char *sdp, size_t sdp_len, uint32_t *audio_ssrc, uint32_t *video_ssrc, uint32_t *rtx_ssrc) {
  *audio_ssrc = 0;
  *video_ssrc = 0;
  *rtx_ssrc = 0;
  int current_media = 0; /* 0 = none, 1 = audio, 2 = video */

  size_t pos = 0;
  while (pos < sdp_len) {
    size_t line_start = pos;
    while (pos < sdp_len && sdp[pos] != '\n') {
      pos++;
    }
    size_t line_end = pos;
    if (line_end > line_start && sdp[line_end - 1] == '\r') {
      line_end--;
    }
    if (pos < sdp_len) {
      pos++;
    }

    size_t len = line_end - line_start;
    const char *line = sdp + line_start;

    if (len >= 7 && memcmp(line, "m=audio", 7) == 0) {
      current_media = 1;
    } else if (len >= 7 && memcmp(line, "m=video", 7) == 0) {
      current_media = 2;
    } else if (len >= 7 && memcmp(line, "a=ssrc:", 7) == 0) {
      if (len >= 12 && memcmp(line, "a=ssrc-group", 12) == 0) {
        continue;
      }

      /* Extract SSRC value safely */
      char *endptr;
      uint32_t ssrc = (uint32_t)strtoul(line + 7, &endptr, 10);

      /* If strtoul didn't consume any digits, skip this line entirely */
      if (endptr == line + 7 || ssrc == 0) {
        continue;
      }

      if (current_media == 1 && *audio_ssrc == 0) {
        *audio_ssrc = ssrc;
      } else if (current_media == 2) {
        if (*video_ssrc == 0) {
          *video_ssrc = ssrc;
        } else if (*rtx_ssrc == 0 && ssrc != *video_ssrc) {
          *rtx_ssrc = ssrc;
        }
      }
    }
  }
}

static bool build_and_send_answer(int fd, sfu_signaling_server_t *s, const char *client_ufrag, const char *offer_sdp, size_t offer_sdp_len, uint32_t ans_audio,
                                  uint32_t ans_video, uint32_t ans_rtx) {
  char answer[SFU_SIGNALING_SDP_CAP];
  int answer_len = sfu_sdp_build_answer(offer_sdp, offer_sdp_len, s->media_host, s->media_port, s->ice_creds->ufrag, s->ice_creds->pwd,
                                        s->dtls_ctx->fingerprint, ans_audio, ans_video, ans_rtx, answer, sizeof(answer));
  if (answer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build SDP answer (fd=%d, ufrag=%s)", fd, client_ufrag);
    return false;
  }
  SFU_LOG_INFO("signaling: raw answer built: %d bytes (fd=%d, ufrag=%s, audio_ssrc=%u, video_ssrc=%u, rtx_ssrc=%u)", answer_len, fd, client_ufrag, ans_audio,
               ans_video, ans_rtx);

  char escaped[SFU_SIGNALING_JSON_CAP];
  int escaped_len = sfu_json_escape(answer, (size_t)answer_len, escaped, sizeof(escaped));
  if (escaped_len < 0) {
    SFU_LOG_WARN("signaling: answer too large to escape into JSON response (fd=%d, ufrag=%s)", fd, client_ufrag);
    return false;
  }

  char response[SFU_SIGNALING_JSON_CAP + 64];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"answer\",\"sdp\":\"%s\"}", escaped);
  if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
    SFU_LOG_WARN("signaling: response too large to send (fd=%d, ufrag=%s)", fd, client_ufrag);
    return false;
  }

  if (sfu_ws_send_text(fd, response, (size_t)response_len) != 0) {
    SFU_LOG_WARN("signaling: failed to send answer over WebSocket (fd=%d, ufrag=%s)", fd, client_ufrag);
    return false;
  }

  SFU_LOG_INFO("signaling: sent SDP answer (%d bytes) for a %zu-byte offer (fd=%d, ufrag=%s)", response_len, offer_sdp_len, fd, client_ufrag);
  return true;
}

/**
 * @brief Thread-safe public trigger to invoke room-wide renegotiation.
 *        Called directly by workers once a peer finishes DTLS handshake steps.
 */
void sfu_signaling_trigger_renegotiation(sfu_room_t *room, const char *exclude_ufrag) {
  if (!room || !exclude_ufrag || exclude_ufrag[0] == '\0') {
    return;
  }

  sfu_publisher_snapshot_t snaps[SFU_ROOM_MAX_PEERS];
  uint32_t n = sfu_room_snapshot_other_publishers(room, exclude_ufrag, snaps, SFU_ROOM_MAX_PEERS);

  for (uint32_t i = 0; i < n; i++) {
    const char *reneg_msg = "{\"type\":\"renegotiate\"}";

    if (sfu_ws_send_text(snaps[i].fd, reneg_msg, strlen(reneg_msg)) == 0) {
      SFU_LOG_INFO("signaling: safely requested renegotiation from ufrag=%s (fd=%d) now that ufrag=%s DTLS keys are hot", snaps[i].ufrag, snaps[i].fd,
                   exclude_ufrag);
    } else {
      SFU_LOG_WARN("signaling: failed to send secure renegotiate request to fd=%d", snaps[i].fd);
    }

    SFU_FREE(snaps[i].offer_sdp);
  }
}

static void handle_offer(int fd, sfu_signaling_server_t *s, sfu_room_t *room, const char *client_ufrag, const char *sdp, int sdp_len) {
  uint32_t off_audio = 0, off_video = 0, off_rtx = 0;
  extract_sdp_ssrcs(sdp, (size_t)sdp_len, &off_audio, &off_video, &off_rtx);

  if (room && client_ufrag[0] != '\0') {
    sfu_room_publish(room, client_ufrag, fd, sdp, (size_t)sdp_len, off_audio, off_video, off_rtx);
    if (off_audio != 0 || off_video != 0) {
      SFU_LOG_INFO("signaling: captured publisher SSRCs for room %" PRIu64 " ufrag=%s (audio=%u, video=%u, rtx=%u)", room->room_id, client_ufrag, off_audio,
                   off_video, off_rtx);
    }
  }

  uint32_t ans_audio = 0, ans_video = 0, ans_rtx = 0;
  if (room && client_ufrag[0] != '\0') {
    sfu_room_get_other_publisher_ssrcs(room, client_ufrag, &ans_audio, &ans_video, &ans_rtx);
  }

  build_and_send_answer(fd, s, client_ufrag, sdp, (size_t)sdp_len, ans_audio, ans_video, ans_rtx);
}

static void publish_join_event_to_nats(sfu_signaling_server_t *s, uint64_t room_id, const char *peer_ip) {
  (void)s;
  SFU_LOG_INFO(
      "INTEGRATION: [NATS Publish] Topic: sfu.room.join | Payload: "
      "{\"room\": %" PRIu64 ", \"ip\": \"%s\"}",
      room_id, peer_ip);
}
static int extract_header_val(const char *handshake, const char *header_name, char *out_val, size_t out_len) {
  char search_str[128];
  snprintf(search_str, sizeof(search_str), "\r\n%s:", header_name);

  const char *pos = strcasestr(handshake, search_str);
  if (!pos) {
    snprintf(search_str, sizeof(search_str), "%s:", header_name);
    if (strncmp(handshake, search_str, strlen(search_str)) == 0) {
      pos = handshake;
    } else {
      return -1;
    }
  } else {
    pos += 2;
  }

  pos += strlen(header_name) + 1;

  while (*pos == ' ' || *pos == '\t') {
    pos++;
  }

  const char *end = strstr(pos, "\r\n");
  if (!end) {
    return -1;
  }

  size_t len = (size_t)(end - pos);
  if (len >= out_len) {
    len = out_len - 1;
  }

  strncpy(out_val, pos, len);
  out_val[len] = '\0';

  char *comma = strchr(out_val, ',');
  if (comma) {
    *comma = '\0';
  }

  return 0;
}

typedef struct {
  uv_poll_t poll_handle;
  int fd;
  bool handshake_done;
  char peer_ip[64];
  int ip_detected_from_header;
  sfu_room_t *joined_room;
  uint64_t joined_room_id;
  char client_ufrag[32];
  sfu_signaling_server_t *server;
} sfu_client_conn_t;

/* Callback executed when a client handle is fully closed */
static void on_client_close(uv_handle_t *handle) {
  sfu_client_conn_t *c = (sfu_client_conn_t *)handle->data;
  if (c->joined_room && c->client_ufrag[0] != '\0') {
    sfu_room_unpublish(c->joined_room, c->client_ufrag);
  }
  close(c->fd);
  SFU_FREE(c);
}

/* Helper to initiate closing a client connection */
static void disconnect_client(sfu_client_conn_t *c) {
  if (!uv_is_closing((uv_handle_t *)&c->poll_handle)) {
    uv_poll_stop(&c->poll_handle);
    uv_close((uv_handle_t *)&c->poll_handle, on_client_close);
  }
}

/* Main I/O callback for active client WebSocket connections */
static void on_client_readable(uv_poll_t *handle, int status, int events) {
  sfu_client_conn_t *c = (sfu_client_conn_t *)handle->data;
  sfu_signaling_server_t *s = c->server;

  if (status < 0 || (events & UV_DISCONNECT)) {
    disconnect_client(c);
    return;
  }

  if (events & UV_READABLE) {
    if (!c->handshake_done) {
      char peek_buf[2048];
      ssize_t peek_len = recv(c->fd, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);
      if (peek_len > 0) {
        peek_buf[peek_len] = '\0';
        if (extract_header_val(peek_buf, "X-Real-IP", c->peer_ip, sizeof(c->peer_ip)) == 0) {
          c->ip_detected_from_header = 1;
        } else if (extract_header_val(peek_buf, "X-Forwarded-For", c->peer_ip, sizeof(c->peer_ip)) == 0) {
          c->ip_detected_from_header = 1;
        }
      }

      if (sfu_ws_handshake(c->fd) != 0) {
        SFU_LOG_WARN("signaling: WebSocket handshake failed");
        disconnect_client(c);
      } else {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        if (getpeername(c->fd, (struct sockaddr *)&peer_addr, &peer_addr_len) == 0) {
          if (!c->ip_detected_from_header) {
            if (peer_addr.ss_family == AF_INET) {
              struct sockaddr_in *s4 = (struct sockaddr_in *)&peer_addr;
              inet_ntop(AF_INET, &s4->sin_addr, c->peer_ip, sizeof(c->peer_ip));
            } else if (peer_addr.ss_family == AF_INET6) {
              struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&peer_addr;
              inet_ntop(AF_INET6, &s6->sin6_addr, c->peer_ip, sizeof(c->peer_ip));
            }
          }
        }
        c->handshake_done = true;
        SFU_LOG_INFO("signaling: peer joined from IP: %s (Detected from header: %s)", c->peer_ip, c->ip_detected_from_header ? "YES" : "NO");
      }
    } else {
      char buf[SFU_SIGNALING_RECV_CAP];
      ssize_t n = sfu_ws_recv_text(c->fd, buf, sizeof(buf));
      if (n <= 0) {
        disconnect_client(c);
      } else {
        char type[32];
        if (sfu_json_extract_string(buf, "type", type, sizeof(type)) >= 0) {
          if (strcmp(type, "join") == 0) {
            char room_str[64] = {0};
            uint64_t room_id = 0;

            if (sfu_json_extract_string(buf, "room", room_str, sizeof(room_str)) >= 0) {
              room_id = (uint64_t)strtoull(room_str, NULL, 10);
            }

            if (room_id == 0) {
              sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"invalid_room\"}", 41);
            } else {
              sfu_room_t *room = sfu_room_registry_get_or_create(s->room_registry, room_id);
              if (!room) {
                sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"room_creation_failed\"}", 49);
              } else {
                c->joined_room = room;
                c->joined_room_id = room_id;
                publish_join_event_to_nats(s, room_id, c->peer_ip);

                SFU_LOG_INFO("signaling: peer %s joined room_id=%" PRIu64, c->peer_ip, room_id);

                char response[128];
                snprintf(response, sizeof(response), "{\"type\":\"joined\",\"room\":\"%" PRIu64 "\"}", room_id);
                sfu_ws_send_text(c->fd, response, strlen(response));
              }
            }
          } else if (strcmp(type, "offer") == 0) {
            if (!c->joined_room) {
              SFU_LOG_WARN("signaling: offer received before join completed for peer %s", c->peer_ip);
              sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"must_join_room_first\"}", 49);
            } else {
              char sdp[SFU_SIGNALING_SDP_CAP];
              int sdp_len = sfu_json_extract_string(buf, "sdp", sdp, sizeof(sdp));
              if (sdp_len >= 0) {
                bool have_ufrag = extract_sdp_ice_ufrag(sdp, (size_t)sdp_len, c->client_ufrag, sizeof(c->client_ufrag));
                if (have_ufrag) {
                  sfu_register_ufrag_room(s->routing_table, c->client_ufrag, c->joined_room);
                  SFU_LOG_INFO("signaling: registered client ufrag=%s -> room_id=%" PRIu64, c->client_ufrag, c->joined_room_id);
                } else {
                  c->client_ufrag[0] = '\0';
                }
                handle_offer(c->fd, s, c->joined_room, c->client_ufrag, sdp, sdp_len);
              }
            }
          } else {
            SFU_LOG_DEBUG("signaling: unrecognized message type \"%s\" from peer %s", type, c->peer_ip);
          }
        }
      }
    }
  }
}

/* Callback executed when the server listening socket accepts a new connection */
static void on_server_readable(uv_poll_t *handle, int status, int events) {
  sfu_signaling_server_t *s = (sfu_signaling_server_t *)handle->data;

  if (status < 0) {
    SFU_LOG_ERROR("signaling: listen socket error: %s", uv_strerror(status));
    return;
  }

  if (events & UV_READABLE) {
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd >= 0) {
      /* Set non-blocking and TCP_NODELAY for optimal WebSocket latency */
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

      sfu_client_conn_t *c = (sfu_client_conn_t *)SFU_MALLOC(sizeof(sfu_client_conn_t));
      memset(c, 0, sizeof(sfu_client_conn_t));
      c->fd = fd;
      c->server = s;
      c->handshake_done = false;
      strcpy(c->peer_ip, "unknown");

      /* Initialize a new poll handle for this client's socket */
      uv_poll_init_socket(handle->loop, &c->poll_handle, fd);
      c->poll_handle.data = c;
      uv_poll_start(&c->poll_handle, UV_READABLE, on_client_readable);
    }
  }
}

/* Helper callback to cleanly shut down open handles on server exit */
static void on_shutdown_walk(uv_handle_t *handle, void *arg) {
  if (!uv_is_closing(handle)) {
    uv_close(handle, NULL);
  }
}

/* The primary thread entry point for the libuv event loop */
static void *signaling_loop_main(void *arg) {
  sfu_signaling_server_t *s = (sfu_signaling_server_t *)arg;

  uv_loop_t loop;
  uv_loop_init(&loop);

  uv_poll_t listen_poll;
  uv_poll_init_socket(&loop, &listen_poll, s->listen_fd);
  listen_poll.data = s;
  uv_poll_start(&listen_poll, UV_READABLE, on_server_readable);

  /* Run the event loop while the server is active */
  while (s->running) {
    /* uv_run with UV_RUN_ONCE allows periodic checking of s->running */
    uv_run(&loop, UV_RUN_ONCE);
  }

  /* Clean up all active client connections during server shutdown */
  uv_walk(&loop, on_shutdown_walk, NULL);
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);

  return NULL;
}

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port, const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds, const sfu_dtls_ctx_t *dtls_ctx, sfu_session_table_t *sessions,
                               sfu_room_registry_t *room_registry, sfu_routing_table_t *routing_table) {
  memset(s, 0, sizeof(*s));
  strncpy(s->media_host, media_host, sizeof(s->media_host) - 1);
  s->media_port = media_port;
  s->ice_creds = ice_creds;
  s->dtls_ctx = dtls_ctx;
  s->sessions = sessions;
  s->room_registry = room_registry;
  s->routing_table = routing_table;

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
  if (pthread_create(&s->thread, NULL, signaling_loop_main, s) != 0) {
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
