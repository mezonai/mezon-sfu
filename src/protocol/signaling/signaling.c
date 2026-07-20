#include "protocol/signaling/signaling.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
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

#define SFU_MAX_UFRAG_ROOM_MAPPINGS 2048

typedef struct {
  char ufrag[32];
  sfu_room_t *room;
} sfu_ufrag_room_map_entry_t;

static sfu_ufrag_room_map_entry_t g_ufrag_room_maps[SFU_MAX_UFRAG_ROOM_MAPPINGS];
static int g_ufrag_room_map_count = 0;
static pthread_mutex_t g_ufrag_room_map_mutex = PTHREAD_MUTEX_INITIALIZER;

void sfu_register_ufrag_room(const char *client_ufrag, sfu_room_t *room) {
  pthread_mutex_lock(&g_ufrag_room_map_mutex);

  for (int i = 0; i < g_ufrag_room_map_count; i++) {
    if (strcmp(g_ufrag_room_maps[i].ufrag, client_ufrag) == 0) {
      g_ufrag_room_maps[i].room = room;
      pthread_mutex_unlock(&g_ufrag_room_map_mutex);
      return;
    }
  }

  if (g_ufrag_room_map_count < SFU_MAX_UFRAG_ROOM_MAPPINGS) {
    strncpy(g_ufrag_room_maps[g_ufrag_room_map_count].ufrag, client_ufrag, sizeof(g_ufrag_room_maps[0].ufrag) - 1);
    g_ufrag_room_maps[g_ufrag_room_map_count].ufrag[sizeof(g_ufrag_room_maps[0].ufrag) - 1] = '\0';
    g_ufrag_room_maps[g_ufrag_room_map_count].room = room;
    g_ufrag_room_map_count++;
  } else {
    SFU_LOG_ERROR(
        "ufrag->room table FULL (%d entries) -- cannot register ufrag=%s, "
        "this peer's media will NOT bind to a room. See KNOWN LIMITATION "
        "in signaling.c: table has no eviction policy yet.",
        SFU_MAX_UFRAG_ROOM_MAPPINGS, client_ufrag);
  }

  pthread_mutex_unlock(&g_ufrag_room_map_mutex);
}

bool sfu_lookup_ufrag_room(const char *client_ufrag, sfu_room_t **out_room) {
  pthread_mutex_lock(&g_ufrag_room_map_mutex);
  for (int i = 0; i < g_ufrag_room_map_count; i++) {
    if (strcmp(g_ufrag_room_maps[i].ufrag, client_ufrag) == 0) {
      *out_room = g_ufrag_room_maps[i].room;
      pthread_mutex_unlock(&g_ufrag_room_map_mutex);
      return true;
    }
  }
  pthread_mutex_unlock(&g_ufrag_room_map_mutex);
  return false;
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

static bool room_has_this_publisher(sfu_room_t *room, const char *client_ufrag) {
  sfu_publisher_snapshot_t snaps[SFU_ROOM_MAX_PEERS];
  // Passing NULL or empty string to snapshot matches EVERYONE in the room
  uint32_t n = sfu_room_snapshot_other_publishers(room, "", snaps, SFU_ROOM_MAX_PEERS);

  bool found = false;
  for (uint32_t i = 0; i < n; i++) {
    if ((snaps[i].ufrag[0] != 0) && strcmp(snaps[i].ufrag, client_ufrag) == 0) {
      found = true;
    }
    // Remember to free the memory allocated for the snapshot SDPs!
    if (snaps[i].offer_sdp) {
      SFU_FREE(snaps[i].offer_sdp);
    }
  }
  return found;
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

static void push_updated_answers_to_others(sfu_signaling_server_t *s, sfu_room_t *room, const char *exclude_ufrag) {
  sfu_publisher_snapshot_t snaps[SFU_ROOM_MAX_PEERS];
  uint32_t n = sfu_room_snapshot_other_publishers(room, exclude_ufrag, snaps, SFU_ROOM_MAX_PEERS);

  for (uint32_t i = 0; i < n; i++) {
    const char *reneg_msg = "{\"type\":\"renegotiate\"}";

    if (sfu_ws_send_text(snaps[i].fd, reneg_msg, strlen(reneg_msg)) == 0) {
      SFU_LOG_INFO("signaling: requested renegotiation from ufrag=%s (fd=%d) because ufrag=%s published new media", snaps[i].ufrag, snaps[i].fd, exclude_ufrag);
    } else {
      SFU_LOG_WARN("signaling: failed to send renegotiate request to fd=%d", snaps[i].fd);
    }

    SFU_FREE(snaps[i].offer_sdp);
  }
}
static void handle_offer(int fd, sfu_signaling_server_t *s, sfu_room_t *room, const char *client_ufrag, const char *sdp, int sdp_len) {
  uint32_t off_audio = 0, off_video = 0, off_rtx = 0;
  extract_sdp_ssrcs(sdp, (size_t)sdp_len, &off_audio, &off_video, &off_rtx);

  int was_already_publishing = 0;
  if (room && client_ufrag[0] != '\0') {
    was_already_publishing = room_has_this_publisher(room, client_ufrag);
  }

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

  if (room && client_ufrag[0] != '\0' && (off_audio != 0 || off_video != 0)) {
    if (!was_already_publishing) {
      push_updated_answers_to_others(s, room, client_ufrag);
    }
  }
}

static void publish_join_event_to_nats(sfu_signaling_server_t *s, uint64_t room_id, const char *peer_ip) {
  (void)s;
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

static void *conn_thread_main(void *arg) {
  conn_ctx_t *ctx = (conn_ctx_t *)arg;
  int fd = ctx->fd;
  sfu_signaling_server_t *s = ctx->server;
  SFU_FREE(ctx);

  char peer_ip[64] = "unknown";
  int ip_detected_from_header = 0;

  char peek_buf[2048];
  ssize_t peek_len = recv(fd, peek_buf, sizeof(peek_buf) - 1, MSG_PEEK);
  if (peek_len > 0) {
    peek_buf[peek_len] = '\0';

    if (extract_header_val(peek_buf, "X-Real-IP", peer_ip, sizeof(peer_ip)) == 0) {
      ip_detected_from_header = 1;
    } else if (extract_header_val(peek_buf, "X-Forwarded-For", peer_ip, sizeof(peer_ip)) == 0) {
      ip_detected_from_header = 1;
    }
  }

  if (sfu_ws_handshake(fd) != 0) {
    SFU_LOG_WARN("signaling: WebSocket handshake failed");
    close(fd);
    return NULL;
  }

  struct sockaddr_storage peer_addr;
  socklen_t peer_addr_len = sizeof(peer_addr);
  if (getpeername(fd, (struct sockaddr *)&peer_addr, &peer_addr_len) != 0) {
    close(fd);
    return NULL;
  }

  if (!ip_detected_from_header) {
    if (peer_addr.ss_family == AF_INET) {
      struct sockaddr_in *s4 = (struct sockaddr_in *)&peer_addr;
      inet_ntop(AF_INET, &s4->sin_addr, peer_ip, sizeof(peer_ip));
    } else if (peer_addr.ss_family == AF_INET6) {
      struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&peer_addr;
      inet_ntop(AF_INET6, &s6->sin6_addr, peer_ip, sizeof(peer_ip));
    }
  }

  SFU_LOG_INFO("signaling: peer joined from IP: %s (Detected from header: %s)", peer_ip, ip_detected_from_header ? "YES" : "NO");

  sfu_room_t *joined_room = NULL;
  uint64_t joined_room_id = 0;
  char client_ufrag[32] = {0};

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

      sfu_room_t *room = sfu_room_registry_get_or_create(s->room_registry, room_id);
      if (!room) {
        sfu_ws_send_text(fd, "{\"type\":\"error\",\"message\":\"room_creation_failed\"}", 49);
        continue;
      }

      joined_room = room;
      joined_room_id = room_id;
      publish_join_event_to_nats(s, room_id, peer_ip);

      SFU_LOG_INFO("signaling: peer %s joined room_id=%" PRIu64
                   " (will bind to a specific "
                   "media session once its offer's client ufrag is seen)",
                   peer_ip, room_id);

      char response[128];
      snprintf(response, sizeof(response), "{\"type\":\"joined\",\"room\":\"%" PRIu64 "\"}", room_id);
      sfu_ws_send_text(fd, response, strlen(response));

    } else if (strcmp(type, "offer") == 0) {
      if (!joined_room) {
        SFU_LOG_WARN("signaling: offer received before join completed for peer %s", peer_ip);
        sfu_ws_send_text(fd, "{\"type\":\"error\",\"message\":\"must_join_room_first\"}", 49);
        continue;
      }

      char sdp[SFU_SIGNALING_SDP_CAP];
      int sdp_len = sfu_json_extract_string(buf, "sdp", sdp, sizeof(sdp));
      if (sdp_len < 0) {
        SFU_LOG_WARN("signaling: offer message with no sdp field from peer %s", peer_ip);
        continue;
      }

      bool have_ufrag = extract_sdp_ice_ufrag(sdp, (size_t)sdp_len, client_ufrag, sizeof(client_ufrag));
      if (have_ufrag) {
        sfu_register_ufrag_room(client_ufrag, joined_room);
        SFU_LOG_INFO("signaling: registered client ufrag=%s -> room_id=%" PRIu64
                     " for peer %s "
                     "(media path will bind this at first authenticated STUN request)",
                     client_ufrag, joined_room_id, peer_ip);
      } else {
        SFU_LOG_WARN(
            "signaling: could not find a=ice-ufrag in offer SDP from peer %s -- "
            "this peer's media session will NOT be bound to a room automatically. "
            "Check that the offer actually contains an m= line with ICE credentials.",
            peer_ip);
        client_ufrag[0] = '\0';
      }

      handle_offer(fd, s, joined_room, client_ufrag, sdp, sdp_len);
    } else {
      SFU_LOG_DEBUG("signaling: unrecognized message type \"%s\" from peer %s", type, peer_ip);
    }
  }

  if (joined_room && client_ufrag[0] != '\0') {
    sfu_room_unpublish(joined_room, client_ufrag);
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
      continue;
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
