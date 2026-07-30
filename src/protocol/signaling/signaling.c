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
#include "room/room_media_graph.h"
#include "room/room_registry.h"
#include "runtime/routing_context.h"
#include "util/alloc.h"
#include "util/log.h"

void sfu_register_ufrag_room(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd) {
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
    entry->fd = fd;

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

/* Server-offerer architecture: the SFU is always the one running one signaling
   server instance per process (see main.c), so a module-level pointer lets
   sfu_signaling_trigger_renegotiation() -- invoked from worker/dispatch.c with
   only a room handle -- reach media_host/ice_creds/dtls_ctx to build a fresh
   offer without threading the server pointer through the whole call chain. */
static sfu_signaling_server_t *g_signaling_server = NULL;

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

/* Extracts VP8 and RTX dynamic payload type numbers from an SDP offer */
static void extract_sdp_video_pts(const char *sdp, size_t sdp_len, uint8_t *video_pt, uint8_t *rtx_pt) {
  *video_pt = 0;
  *rtx_pt = 0;
  if (!sdp || sdp_len == 0) {
    return;
  }

  int current_media = 0;
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
    } else if (current_media == 2 && len >= 9 && memcmp(line, "a=rtpmap:", 9) == 0) {
      char *endptr;
      unsigned long pt = strtoul(line + 9, &endptr, 10);
      if (endptr > line + 9 && *endptr == ' ' && pt < 128) {
        const char *codec = endptr + 1;
        size_t codec_len = len - (size_t)(codec - line);
        if (codec_len >= 3 && (memcmp(codec, "VP8", 3) == 0 || memcmp(codec, "vp8", 3) == 0)) {
          *video_pt = (uint8_t)pt;
        } else if (codec_len >= 3 && (memcmp(codec, "rtx", 3) == 0 || memcmp(codec, "RTX", 3) == 0)) {
          *rtx_pt = (uint8_t)pt;
        }
      }
    }
  }
}

static bool build_and_send_initial_offer(int fd, sfu_signaling_server_t *s) {
  char offer[SFU_SIGNALING_SDP_CAP];

  int offer_len =
      sfu_sdp_build_initial_offer(s->media_host, s->media_port, s->ice_creds->ufrag, s->ice_creds->pwd, s->dtls_ctx->fingerprint, offer, sizeof(offer));

  if (offer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build initial SDP offer (fd=%d)", fd);
    return false;
  }

  char escaped[SFU_SIGNALING_JSON_CAP];
  int escaped_len = sfu_json_escape(offer, (size_t)offer_len, escaped, sizeof(escaped));

  if (escaped_len < 0) {
    SFU_LOG_WARN("signaling: offer too large (fd=%d)", fd);
    return false;
  }

  char response[SFU_SIGNALING_JSON_CAP + 64];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"offer\",\"sdp\":\"%s\"}", escaped);

  if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
    return false;
  }

  if (sfu_ws_send_text(fd, response, (size_t)response_len) != 0) {
    SFU_LOG_WARN("signaling: failed to send initial offer (fd=%d)", fd);
    return false;
  }

  SFU_LOG_INFO("signaling: sent initial server offer (fd=%d)", fd);

  return true;
}

static bool build_and_send_offer(int fd, sfu_peer_session_t *session, sfu_signaling_server_t *s) {
  char offer[SFU_SIGNALING_SDP_CAP];
  int offer_len =
      sfu_sdp_build_offer(session, s->media_host, s->media_port, s->ice_creds->ufrag, s->ice_creds->pwd, s->dtls_ctx->fingerprint, offer, sizeof(offer));
  if (offer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build server-initiated SDP offer (fd=%d)", fd);
    return false;
  }

  char escaped[SFU_SIGNALING_JSON_CAP];
  int escaped_len = sfu_json_escape(offer, (size_t)offer_len, escaped, sizeof(escaped));
  if (escaped_len < 0) {
    SFU_LOG_WARN("signaling: offer too large to escape into JSON message (fd=%d)", fd);
    return false;
  }

  char response[SFU_SIGNALING_JSON_CAP + 64];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"offer\",\"sdp\":\"%s\"}", escaped);
  if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
    SFU_LOG_WARN("signaling: offer message too large to send (fd=%d)", fd);
    return false;
  }

  if (sfu_ws_send_text(fd, response, (size_t)response_len) != 0) {
    SFU_LOG_WARN("signaling: failed to send server-initiated offer over WebSocket (fd=%d)", fd);
    return false;
  }

  return true;
}

void sfu_signaling_trigger_renegotiation(sfu_room_t *room) {
  if (!room || !g_signaling_server) {
    return;
  }

  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *session = room->peers[i];

    if (!session) {
      continue;
    }

    if (build_and_send_offer(session->fd, session, g_signaling_server)) {
      SFU_LOG_INFO("signaling: sent renegotiation offer to ufrag=%s (fd=%d)", session->ufrag, session->fd);
    } else {
      SFU_LOG_WARN("signaling: failed to send renegotiation offer to fd=%d", g_signaling_server->listen_fd);
    }
  }
  pthread_mutex_unlock(&room->lock);
}

static void handle_answer(sfu_peer_session_t *session, const char *sdp, int sdp_len) {
  if (!session) {
    return;
  }

  uint32_t audio_ssrc = 0;
  uint32_t video_ssrc = 0;
  uint32_t rtx_ssrc = 0;

  extract_sdp_ssrcs(sdp, (size_t)sdp_len, &audio_ssrc, &video_ssrc, &rtx_ssrc);

  uint8_t video_pt = 0;
  uint8_t rtx_pt = 0;

  extract_sdp_video_pts(sdp, (size_t)sdp_len, &video_pt, &rtx_pt);

  session->uplink_audio.ssrc = audio_ssrc;
  session->uplink_audio.active = (audio_ssrc != 0);

  session->uplink_video.ssrc = video_ssrc;
  session->uplink_video.rtx_ssrc = rtx_ssrc;
  session->uplink_video.active = (video_ssrc != 0);

  session->uplink_video.payload_type = video_pt;
  session->uplink_video.rtx_payload_type = rtx_pt;

  for (int i = 0; i < 128; i++) {
    session->pt_map[i] = (uint8_t)i;
  }

  if (video_pt != 0) {
    session->pt_map[96] = video_pt;
  }

  if (rtx_pt != 0) {
    session->pt_map[97] = rtx_pt;
  }

  SFU_LOG_DEBUG("answer: ufrag=%s audio_ssrc=%u video_ssrc=%u rtx_ssrc=%u video_pt=%u rtx_pt=%u", session->ufrag, audio_ssrc, video_ssrc, rtx_ssrc, video_pt,
                rtx_pt);
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

static void on_client_close(uv_handle_t *handle) {
  sfu_client_conn_t *c = (sfu_client_conn_t *)handle->data;

  sfu_peer_session_t *session = c->session;
  if (!session && c->client_ufrag[0] != '\0') {
    session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  }

  if (session) {
    if (session->room) {
      room_remove_peer(session->room, session);
    }
    sfu_session_table_remove(c->server->sessions, session);
  }

  close(c->fd);
  SFU_FREE(c);
}

static void disconnect_client(sfu_client_conn_t *c) {
  if (!uv_is_closing((uv_handle_t *)&c->poll_handle)) {
    uv_poll_stop(&c->poll_handle);
    uv_close((uv_handle_t *)&c->poll_handle, on_client_close);
  }
}

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
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return;
        }
        disconnect_client(c);
      } else if (n == 0) {
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

                if (!build_and_send_initial_offer(c->fd, s)) {
                  SFU_LOG_WARN("signaling: failed to send initial offer (fd=%d)", c->fd);
                }
              }
            }
          } else if (strcmp(type, "answer") == 0) {
            if (!c->joined_room) {
              SFU_LOG_WARN("signaling: answer received before join completed for peer %s", c->peer_ip);
              sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"must_join_room_first\"}", 49);
            } else {
              char sdp[SFU_SIGNALING_SDP_CAP];
              int sdp_len = sfu_json_extract_string(buf, "sdp", sdp, sizeof(sdp));
              if (sdp_len >= 0) {
                bool have_ufrag = extract_sdp_ice_ufrag(sdp, (size_t)sdp_len, c->client_ufrag, sizeof(c->client_ufrag));
                if (have_ufrag) {
                  sfu_register_ufrag_room(s->routing_table, c->client_ufrag, c->joined_room, c->fd);
                  SFU_LOG_INFO("signaling: registered client ufrag=%s -> room_id=%" PRIu64, c->client_ufrag, c->joined_room_id);
                } else {
                  c->client_ufrag[0] = '\0';
                }
                sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(s->sessions, c->client_ufrag);
                if (session) {
                  c->session = session;
                  handle_answer(session, sdp, sdp_len);
                } else {
                  uint32_t audio_ssrc = 0, video_ssrc = 0, rtx_ssrc = 0;
                  uint8_t video_pt = 0, rtx_pt = 0;
                  extract_sdp_ssrcs(sdp, (size_t)sdp_len, &audio_ssrc, &video_ssrc, &rtx_ssrc);
                  extract_sdp_video_pts(sdp, (size_t)sdp_len, &video_pt, &rtx_pt);

                  sfu_routing_table_set_pending_answer(s->routing_table, c->client_ufrag, audio_ssrc, video_ssrc, rtx_ssrc, video_pt, rtx_pt);
                  SFU_LOG_WARN("signaling: answer for ufrag=%s arrived before session was created; stashed parsed SSRCs for apply on bind", c->client_ufrag);
                }
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

static void on_server_readable(uv_poll_t *handle, int status, int events) {
  sfu_signaling_server_t *s = (sfu_signaling_server_t *)handle->data;

  if (status < 0) {
    SFU_LOG_ERROR("signaling: listen socket error: %s", uv_strerror(status));
    return;
  }

  if (events & UV_READABLE) {
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd >= 0) {
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

      uv_poll_init_socket(handle->loop, &c->poll_handle, fd);
      c->poll_handle.data = c;
      uv_poll_start(&c->poll_handle, UV_READABLE, on_client_readable);
    }
  }
}

static void on_async_wake(uv_async_t *handle) { uv_stop(handle->loop); }

static void on_shutdown_walk(uv_handle_t *handle, void *arg) {
  if (uv_is_closing(handle)) {
    return;
  }

  sfu_signaling_server_t *s = (sfu_signaling_server_t *)arg;

  if (handle->type == UV_POLL && handle->data != NULL && handle->data != s) {
    uv_poll_stop((uv_poll_t *)handle);
    uv_close(handle, on_client_close);
  } else {
    uv_close(handle, NULL);
  }
}

static void *signaling_loop_main(void *arg) {
  sfu_signaling_server_t *s = (sfu_signaling_server_t *)arg;

  uv_loop_t loop;
  uv_loop_init(&loop);

  uv_async_init(&loop, &s->async_waker, on_async_wake);
  s->async_waker.data = NULL;

  uv_poll_t listen_poll;
  uv_poll_init_socket(&loop, &listen_poll, s->listen_fd);
  listen_poll.data = s;
  uv_poll_start(&listen_poll, UV_READABLE, on_server_readable);

  while (s->running) {
    uv_run(&loop, UV_RUN_DEFAULT);
  }

  uv_walk(&loop, on_shutdown_walk, s);

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

  g_signaling_server = s;

  SFU_LOG_INFO("signaling server listening on ws://0.0.0.0:%u", listen_port);
  return 0;
}

void sfu_signaling_server_stop(sfu_signaling_server_t *s) {
  if (!s || !s->running) {
    return;
  }

  s->running = 0;
  uv_async_send(&s->async_waker);

  pthread_join(s->thread, NULL);
  close(s->listen_fd);

  if (g_signaling_server == s) {
    g_signaling_server = NULL;
  }
}
