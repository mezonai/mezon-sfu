#include "protocol/signaling/signaling.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/base64.h>
#include <openssl/digest.h>
#include <openssl/hmac.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>
#include "api/hook/producer.h"
#include "config/config.h"
#include "peer/session.h"
#include "protocol/signaling/handshake.h"
#include "protocol/signaling/json_lite.h"
#include "protocol/signaling/sdp.h"
#include "protocol/websocket/ws.h"
#include "room/room_media_graph.h"
#include "room/room_registry.h"
#include "runtime/routing_context.h"
#include "runtime/scheduler.h"
#include "runtime/timer.h"
#include "util/alloc.h"
#include "util/log.h"

uint32_t generate_unique_id(void) {
  static atomic_uint_fast32_t counter = 0;
  return atomic_fetch_add(&counter, 1) + 1;
}

static bool build_and_send_joined_response(sfu_client_conn_t *c, uint64_t room_id) {
  if (!c || !c->server) {
    return false;
  }

  char turn_secret[64] = {0};

  sfu_signaling_server_t *s = c->server;
  char response[1024];
  int response_len = 0;

  if (turn_secret[0] != '\0' && s->media_host[0] != '\0') {
    char turn_user[64] = {0};
    char turn_pass[64] = {0};

    sfu_signaling_generate_turn_credentials(turn_secret, c->peer_ip, turn_user, sizeof(turn_user), turn_pass, sizeof(turn_pass), 86400);

    response_len = snprintf(response, sizeof(response),
                            "{"
                            "\"type\":\"joined\","
                            "\"room\":\"%" PRIu64
                            "\","
                            "\"iceServers\":["
                            "{\"urls\":\"stun:%s:3478\"},"
                            "{\"urls\":\"turn:%s:3478?transport=udp\",\"username\":\"%s\",\"credential\":\"%s\"},"
                            "{\"urls\":\"turn:%s:443?transport=tcp\",\"username\":\"%s\",\"credential\":\"%s\"}"
                            "]"
                            "}",
                            room_id, s->media_host, s->media_host, turn_user, turn_pass, s->media_host, turn_user, turn_pass);
  } else {
    const char *host = (s->media_host[0] != '\0') ? s->media_host : "stun.l.google.com";
    uint16_t port = (s->media_host[0] != '\0') ? 3478 : 19302;

    response_len = snprintf(response, sizeof(response),
                            "{"
                            "\"type\":\"joined\","
                            "\"room\":\"%" PRIu64
                            "\","
                            "\"iceServers\":["
                            "{\"urls\":\"stun:%s:%u\"}"
                            "]"
                            "}",
                            room_id, host, port);
  }

  if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
    SFU_LOG_WARN("signaling: joined response message too large (fd=%d)", c->fd);
    return false;
  }

  if (sfu_ws_send_text(c->fd, response, (size_t)response_len) != 0) {
    SFU_LOG_WARN("signaling: failed to send joined response (fd=%d)", c->fd);
    return false;
  }

  return true;
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

static sfu_signaling_server_t *g_signaling_server = NULL;


static bool send_offer_json(int fd, sfu_signaling_server_t *s, size_t offer_len) {
  static const char prefix[] = "{\"type\":\"offer\",\"sdp\":\"";
  size_t prefix_len = sizeof(prefix) - 1;
  if (!s->scratch.json || prefix_len + 3 >= SFU_SIGNALING_JSON_CAP) {
    return false;
  }
  memcpy(s->scratch.json, prefix, prefix_len);
  int escaped_len = sfu_json_escape(s->scratch.sdp, offer_len, s->scratch.json + prefix_len, SFU_SIGNALING_JSON_CAP - prefix_len - 3);
  if (escaped_len < 0) {
    return false;
  }
  size_t response_len = prefix_len + (size_t)escaped_len;
  s->scratch.json[response_len++] = '"';
  s->scratch.json[response_len++] = '}';
  s->scratch.json[response_len] = '\0';
  return sfu_ws_send_text(fd, s->scratch.json, response_len) == 0;
}

static bool build_and_send_initial_offer(int fd, bool is_audience, sfu_signaling_server_t *s) {
  int offer_len = sfu_sdp_build_initial_offer(s->media_host, s->media_port, s->ice_creds->ufrag, s->ice_creds->pwd, s->dtls_ctx->fingerprint, is_audience,
                                              s->scratch.sdp, SFU_SIGNALING_SDP_CAP);
  if (offer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build initial SDP offer (fd=%d)", fd);
    return false;
  }
  if (!send_offer_json(fd, s, (size_t)offer_len)) {
    SFU_LOG_WARN("signaling: failed to send initial offer (fd=%d)", fd);
    return false;
  }
  SFU_LOG_INFO("signaling: sent initial server offer (fd=%d)", fd);
  return true;
}

static bool build_and_send_offer(int fd, sfu_peer_session_t *session, sfu_signaling_server_t *s) {
  int offer_len = sfu_sdp_build_offer(session, s->media_host, s->media_port, s->ice_creds->ufrag, s->ice_creds->pwd, s->dtls_ctx->fingerprint, s->scratch.sdp,
                                      SFU_SIGNALING_SDP_CAP);
  if (offer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build server-initiated SDP offer (fd=%d)", fd);
    return false;
  }
  if (!send_offer_json(fd, s, (size_t)offer_len)) {
    SFU_LOG_WARN("signaling: failed to send server-initiated offer over WebSocket (fd=%d)", fd);
    return false;
  }
  return true;
}

void sfu_signaling_notify_peer_admitted(sfu_room_t *room, sfu_peer_session_t *peer) {
  if (!room || !peer || !g_signaling_server || peer->room != room) {
    return;
  }
  bool expected = false;
  if (!atomic_compare_exchange_strong_explicit(&peer->membership_announced, &expected, true, memory_order_acq_rel, memory_order_acquire)) {
    return;
  }

  atomic_fetch_add_explicit(&peer->refcount, 1, memory_order_relaxed);
  sfu_membership_queue_t *queue = &g_signaling_server->membership_queue;
  pthread_mutex_lock(&queue->lock);
  if (queue->count >= SFU_MEMBERSHIP_QUEUE_CAP) {
    pthread_mutex_unlock(&queue->lock);
    atomic_store_explicit(&peer->membership_announced, false, memory_order_release);
    sfu_session_release(peer);
    SFU_LOG_WARN("signaling: membership queue full for peer %u", peer->peer_id);
    return;
  }
  queue->items[queue->tail] = peer;
  queue->tail = (queue->tail + 1) % SFU_MEMBERSHIP_QUEUE_CAP;
  queue->count++;
  pthread_mutex_unlock(&queue->lock);
  uv_async_send(&g_signaling_server->renegotiation_waker);
}

void sfu_signaling_trigger_renegotiation(sfu_room_t *room) {
  if (!room || !g_signaling_server) {
    return;
  }

  sfu_peer_session_t *sessions[SFU_ROOM_MAX_PEERS];
  uint32_t count = 0;
  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->peer_count && count < SFU_ROOM_MAX_PEERS; i++) {
    sfu_peer_session_t *session = room->peers[i];
    if (!session || !sfu_session_accepts_work(session)) {
      continue;
    }
    atomic_fetch_add_explicit(&session->refcount, 1, memory_order_relaxed);
    sessions[count++] = session;
  }
  pthread_mutex_unlock(&room->lock);

  for (uint32_t i = 0; i < count; i++) {
    sfu_peer_session_t *session = sessions[i];
    bool enqueue = false;
    pthread_mutex_lock(&session->negotiation_lock);
    session->renegotiation_pending = true;
    if (session->state != SFU_SESSION_ESTABLISHED) {
      pthread_mutex_unlock(&session->negotiation_lock);
      sfu_session_release(session);
      continue;
    }
    if (!session->negotiation_needed) {
      session->negotiation_needed = true;
      enqueue = true;
    }
    pthread_mutex_unlock(&session->negotiation_lock);

    if (enqueue) {
      sfu_renegotiation_queue_t *queue = &g_signaling_server->renegotiation_queue;
      pthread_mutex_lock(&queue->lock);
      if (queue->count < SFU_RENEGOTIATION_QUEUE_CAP) {
        queue->items[queue->tail] = session;
        queue->tail = (queue->tail + 1) % SFU_RENEGOTIATION_QUEUE_CAP;
        queue->count++;
        session = NULL;
      }
      pthread_mutex_unlock(&queue->lock);
    }
    if (session) {
      pthread_mutex_lock(&session->negotiation_lock);
      session->negotiation_needed = false;
      pthread_mutex_unlock(&session->negotiation_lock);
      sfu_session_release(session);
    }
  }
  uv_async_send(&g_signaling_server->renegotiation_waker);
}

static void notify_peer_left(sfu_room_t *room, sfu_peer_session_t *leaving) {
  if (!room || !leaving) {
    return;
  }

  const char *ufrag = (leaving->cold && leaving->cold->ufrag[0] != '\0') ? leaving->cold->ufrag : "";
  const int64_t user_id = leaving->user_id;
  const uint32_t peer_id = leaving->peer_id;

  pthread_mutex_lock(&room->lock);
  uint32_t participant_count = room->peer_count > 0 ? room->peer_count - 1 : 0;
  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == leaving || other->fd < 0 || !sfu_session_accepts_work(other)) {
      continue;
    }

    uint32_t mid_audio = 0;
    uint32_t mid_video = 0;
    uint32_t mid_screen = 0;
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(other);
    if (snap) {
      for (uint32_t j = 0; j < snap->count; j++) {
        if (snap->entries[j].subscriber == leaving) {
          mid_audio = snap->entries[j].mid_audio;
          mid_video = snap->entries[j].mid_video;
          mid_screen = snap->entries[j].mid_screen;
          break;
        }
      }
      sfu_subscriptions_snapshot_release(snap);
    }

    char msg[384];
    int n = snprintf(msg, sizeof(msg),
                     "{\"type\":\"peer_left\",\"participant_count\":%u,\"ufrag\":\"%s\",\"user_id\":\"%" PRId64
                     "\",\"peer_id\":%u,"
                     "\"mid_audio\":%u,\"mid_video\":%u,\"mid_screen\":%u}",
                     participant_count, ufrag, user_id, peer_id, mid_audio, mid_video, mid_screen);
    if (n > 0 && (size_t)n < sizeof(msg)) {
      if (sfu_ws_send_text(other->fd, msg, (size_t)n) != 0) {
        SFU_LOG_WARN("signaling: failed to send peer_left to fd=%d for leaving ufrag=%s", other->fd, ufrag);
      } else {
        SFU_LOG_INFO("signaling: peer_left to fd=%d for ufrag=%s mids=%u/%u/%u", other->fd, ufrag, mid_audio, mid_video, mid_screen);
      }
    }
  }
  pthread_mutex_unlock(&room->lock);
}

static uint8_t extract_sdp_twcc_extmap_id(const char *sdp, size_t sdp_len) {
  static const char k_extmap[] = "a=extmap:";
  static const char k_twcc_uri[] = "transport-wide-cc";
  static const char k_sendonly[] = "a=sendonly";
  static const char m_prefix[] = "m=";

  uint8_t id = 0;
  int in_sendonly = 0;
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

    if (len >= sizeof(m_prefix) - 1 && memcmp(line, m_prefix, sizeof(m_prefix) - 1) == 0) {
      in_sendonly = 0;
      continue;
    }
    if (len == sizeof(k_sendonly) - 1 && memcmp(line, k_sendonly, sizeof(k_sendonly) - 1) == 0) {
      in_sendonly = 1;
      continue;
    }

    if (len < sizeof(k_extmap) - 1 || memcmp(line, k_extmap, sizeof(k_extmap) - 1) != 0) {
      continue;
    }

    char *endptr;
    unsigned long parsed = strtoul(line + sizeof(k_extmap) - 1, &endptr, 10);
    if (endptr == line + sizeof(k_extmap) - 1 || parsed == 0 || parsed > 14) {
      continue;
    }
    if (len - (size_t)(endptr - line) < sizeof(k_twcc_uri) - 1) {
      continue;
    }
    if (memmem(endptr, len - (size_t)(endptr - line), k_twcc_uri, sizeof(k_twcc_uri) - 1)) {
      if (in_sendonly) {
        return (uint8_t)parsed;
      }
      if (id == 0) {
        id = (uint8_t)parsed;
      }
    }
  }
  return id;
}

uint8_t sfu_test_extract_twcc_extmap_id(const char *sdp, size_t sdp_len) { return extract_sdp_twcc_extmap_id(sdp, sdp_len); }

typedef enum sfu_sdp_direction {
  SFU_SDP_DIRECTION_NONE = 0,
  SFU_SDP_DIRECTION_SENDONLY,
  SFU_SDP_DIRECTION_RECVONLY,
  SFU_SDP_DIRECTION_SENDRECV,
  SFU_SDP_DIRECTION_INACTIVE,
} sfu_sdp_direction_t;

typedef struct sfu_answer_section {
  int media_kind;
  int mid;
  sfu_sdp_direction_t direction;
  uint8_t payloads[16];
  uint8_t payload_count;
  sfu_video_codec_t codecs[128];
  uint8_t rtx_apt[128];
  bool is_rtx[128];
  uint32_t ssrcs[4];
  uint8_t ssrc_count;
  uint32_t fid_media_ssrc;
  uint32_t fid_rtx_ssrc;
  uint8_t twcc_extmap_id;
  uint8_t mid_extmap_id;
  bool rejected;
} sfu_answer_section_t;

typedef struct sfu_answer_media {
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t rtx_ssrc;
  uint32_t screen_ssrc;
  uint32_t screen_rtx_ssrc;
  uint8_t video_pt;
  uint8_t rtx_pt;
  uint8_t screen_pt;
  uint8_t screen_rtx_pt;
  sfu_video_codec_t video_codec;
  sfu_video_codec_t screen_codec;
  uint8_t twcc_recv_extmap_id;
  uint8_t twcc_send_extmap_id;
  uint8_t mid_recv_extmap_id;
  bool audio_section_present;
  bool video_section_present;
  bool screen_section_present;
  bool audio_sends;
  bool video_sends;
  bool screen_sends;
} sfu_answer_media_t;

static void answer_section_reset(sfu_answer_section_t *section) {
  memset(section, 0, sizeof(*section));
  section->mid = -1;
  section->direction = SFU_SDP_DIRECTION_SENDRECV;
}

static void answer_section_video_fields(const sfu_answer_section_t *section, uint8_t *video_pt, uint8_t *rtx_pt, sfu_video_codec_t *codec, uint32_t *video_ssrc,
                                        uint32_t *rtx_ssrc) {
  for (uint8_t i = 0; i < section->payload_count; i++) {
    uint8_t pt = section->payloads[i];
    if (!section->is_rtx[pt] && section->codecs[pt] != SFU_VIDEO_CODEC_NONE) {
      *video_pt = pt;
      *codec = section->codecs[pt];
      break;
    }
  }
  if (*video_pt != 0) {
    for (uint8_t pt = 0; pt < 128; pt++) {
      if (section->is_rtx[pt] && section->rtx_apt[pt] == *video_pt) {
        *rtx_pt = pt;
        break;
      }
    }
  }
  if (section->fid_media_ssrc != 0) {
    *video_ssrc = section->fid_media_ssrc;
    *rtx_ssrc = section->fid_rtx_ssrc;
  } else if (section->ssrc_count > 0) {
    *video_ssrc = section->ssrcs[0];
    if (section->ssrc_count > 1) {
      *rtx_ssrc = section->ssrcs[1];
    }
  }
}

static void answer_section_finalize(const sfu_answer_section_t *section, sfu_answer_media_t *media) {
  bool sends = !section->rejected && (section->direction == SFU_SDP_DIRECTION_SENDONLY || section->direction == SFU_SDP_DIRECTION_SENDRECV);
  bool receives = !section->rejected && (section->direction == SFU_SDP_DIRECTION_RECVONLY || section->direction == SFU_SDP_DIRECTION_SENDRECV);

  if (section->media_kind == 1 && section->mid == (int)SFU_LOCAL_AUDIO_MID) {
    media->audio_section_present = true;
    media->audio_sends = sends;
    if (sends && section->ssrc_count > 0) {
      media->audio_ssrc = section->ssrcs[0];
    }
  }

  if (section->media_kind == 2 && section->mid == (int)SFU_LOCAL_CAMERA_MID) {
    media->video_section_present = true;
    media->video_sends = sends;
    if (sends) {
      answer_section_video_fields(section, &media->video_pt, &media->rtx_pt, &media->video_codec, &media->video_ssrc, &media->rtx_ssrc);
      media->twcc_recv_extmap_id = section->twcc_extmap_id;
      media->mid_recv_extmap_id = section->mid_extmap_id;
    }
  }

  if (section->media_kind == 2 && section->mid == (int)SFU_LOCAL_SCREEN_MID) {
    media->screen_section_present = true;
    media->screen_sends = sends;
    if (sends) {
      answer_section_video_fields(section, &media->screen_pt, &media->screen_rtx_pt, &media->screen_codec, &media->screen_ssrc, &media->screen_rtx_ssrc);
      if (media->twcc_recv_extmap_id == 0) {
        media->twcc_recv_extmap_id = section->twcc_extmap_id;
      }
      if (media->mid_recv_extmap_id == 0) {
        media->mid_recv_extmap_id = section->mid_extmap_id;
      }
    }
  }

  if (section->mid >= (int)SFU_REMOTE_MID_BASE && receives && media->mid_recv_extmap_id == 0) {
    media->mid_recv_extmap_id = section->mid_extmap_id;
  }

  if (section->media_kind == 2 && section->mid != (int)SFU_LOCAL_CAMERA_MID && section->mid != (int)SFU_LOCAL_SCREEN_MID && receives &&
      media->twcc_send_extmap_id == 0) {
    media->twcc_send_extmap_id = section->twcc_extmap_id;
  }
}

static bool parse_answer_media(const char *sdp, size_t sdp_len, sfu_answer_media_t *media) {
  if (!sdp || !media) {
    return false;
  }

  memset(media, 0, sizeof(*media));
  sfu_answer_section_t section;
  answer_section_reset(&section);
  bool have_section = false;
  sfu_sdp_direction_t session_direction = SFU_SDP_DIRECTION_SENDRECV;

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

    size_t line_len = line_end - line_start;
    if (line_len == 0 || line_len >= 512) {
      continue;
    }
    char line[512];
    memcpy(line, sdp + line_start, line_len);
    line[line_len] = '\0';

    if (strncmp(line, "m=", 2) == 0) {
      if (have_section) {
        answer_section_finalize(&section, media);
      }
      answer_section_reset(&section);
      section.direction = session_direction;
      have_section = true;
      section.media_kind = strncmp(line, "m=audio", 7) == 0 ? 1 : strncmp(line, "m=video", 7) == 0 ? 2 : 0;
      const char *port_start = strchr(line, ' ');
      if (port_start) {
        section.rejected = strtoul(port_start + 1, NULL, 10) == 0;
      }
      const char *payloads = strstr(line, "SAVPF ");
      if (payloads) {
        payloads += 6;
        while (*payloads != '\0' && section.payload_count < sizeof(section.payloads)) {
          char *endptr;
          unsigned long pt = strtoul(payloads, &endptr, 10);
          if (endptr == payloads || pt > 127) {
            break;
          }
          section.payloads[section.payload_count++] = (uint8_t)pt;
          payloads = endptr;
          while (*payloads == ' ') {
            payloads++;
          }
        }
      }
      continue;
    }
    if (!have_section) {
      if (strcmp(line, "a=sendonly") == 0) {
        session_direction = SFU_SDP_DIRECTION_SENDONLY;
      } else if (strcmp(line, "a=recvonly") == 0) {
        session_direction = SFU_SDP_DIRECTION_RECVONLY;
      } else if (strcmp(line, "a=sendrecv") == 0) {
        session_direction = SFU_SDP_DIRECTION_SENDRECV;
      } else if (strcmp(line, "a=inactive") == 0) {
        session_direction = SFU_SDP_DIRECTION_INACTIVE;
      }
      continue;
    }

    if (strncmp(line, "a=mid:", 6) == 0) {
      section.mid = (int)strtol(line + 6, NULL, 10);
    } else if (strcmp(line, "a=sendonly") == 0) {
      section.direction = SFU_SDP_DIRECTION_SENDONLY;
    } else if (strcmp(line, "a=recvonly") == 0) {
      section.direction = SFU_SDP_DIRECTION_RECVONLY;
    } else if (strcmp(line, "a=sendrecv") == 0) {
      section.direction = SFU_SDP_DIRECTION_SENDRECV;
    } else if (strcmp(line, "a=inactive") == 0) {
      section.direction = SFU_SDP_DIRECTION_INACTIVE;
    } else if (strncmp(line, "a=rtpmap:", 9) == 0) {
      char *endptr;
      unsigned long pt = strtoul(line + 9, &endptr, 10);
      if (endptr != line + 9 && *endptr == ' ' && pt < 128) {
        const char *codec = endptr + 1;
        if (strncasecmp(codec, "VP9", 3) == 0) {
          section.codecs[pt] = SFU_VIDEO_CODEC_VP9;
        } else if (strncasecmp(codec, "AV1", 3) == 0) {
          section.codecs[pt] = SFU_VIDEO_CODEC_AV1;
        } else if (strncasecmp(codec, "VP8", 3) == 0) {
          section.codecs[pt] = SFU_VIDEO_CODEC_VP8;
        } else if (strncasecmp(codec, "rtx", 3) == 0) {
          section.is_rtx[pt] = true;
        }
      }
    } else if (strncmp(line, "a=fmtp:", 7) == 0) {
      char *endptr;
      unsigned long pt = strtoul(line + 7, &endptr, 10);
      char *apt = strstr(endptr, "apt=");
      if (pt < 128 && apt) {
        unsigned long apt_pt = strtoul(apt + 4, NULL, 10);
        if (apt_pt < 128) {
          section.is_rtx[pt] = true;
          section.rtx_apt[pt] = (uint8_t)apt_pt;
        }
      }
    } else if (strncmp(line, "a=ssrc-group:FID ", 17) == 0) {
      unsigned media_ssrc = 0, rtx_ssrc = 0;
      if (sscanf(line + 17, "%u %u", &media_ssrc, &rtx_ssrc) == 2) {
        section.fid_media_ssrc = media_ssrc;
        section.fid_rtx_ssrc = rtx_ssrc;
      }
    } else if (strncmp(line, "a=ssrc:", 7) == 0) {
      unsigned long ssrc = strtoul(line + 7, NULL, 10);
      if (ssrc != 0) {
        bool duplicate = false;
        for (uint8_t i = 0; i < section.ssrc_count; i++) {
          duplicate |= section.ssrcs[i] == (uint32_t)ssrc;
        }
        if (!duplicate && section.ssrc_count < 4) {
          section.ssrcs[section.ssrc_count++] = (uint32_t)ssrc;
        }
      }
    } else if (strncmp(line, "a=extmap:", 9) == 0) {
      char *endptr;
      unsigned long id = strtoul(line + 9, &endptr, 10);
      if (endptr != line + 9 && id > 0 && id < 15) {
        if (strstr(line, "transport-wide-cc")) {
          section.twcc_extmap_id = (uint8_t)id;
        } else if (strstr(line, "urn:ietf:params:rtp-hdrext:sdes:mid")) {
          section.mid_extmap_id = (uint8_t)id;
        }
      }
    }
  }

  if (have_section) {
    answer_section_finalize(&section, media);
  }
  return media->audio_section_present || media->video_section_present || media->screen_section_present;
}

bool sfu_test_parse_answer_media(const char *sdp, size_t sdp_len, uint32_t *audio_ssrc, uint32_t *video_ssrc, uint32_t *rtx_ssrc, uint8_t *video_pt,
                                 uint8_t *rtx_pt, sfu_video_codec_t *video_codec, uint8_t *twcc_recv_extmap_id, uint8_t *twcc_send_extmap_id) {
  sfu_answer_media_t media;
  if (!parse_answer_media(sdp, sdp_len, &media)) {
    return false;
  }
  *audio_ssrc = media.audio_ssrc;
  *video_ssrc = media.video_ssrc;
  *rtx_ssrc = media.rtx_ssrc;
  *video_pt = media.video_pt;
  *rtx_pt = media.rtx_pt;
  *video_codec = media.video_codec;
  *twcc_recv_extmap_id = media.twcc_recv_extmap_id;
  *twcc_send_extmap_id = media.twcc_send_extmap_id;
  return true;
}

bool sfu_test_parse_answer_screen(const char *sdp, size_t sdp_len, uint32_t *screen_ssrc, uint32_t *screen_rtx_ssrc, uint8_t *screen_pt, uint8_t *screen_rtx_pt,
                                  sfu_video_codec_t *screen_codec, uint8_t *mid_extmap_id) {
  sfu_answer_media_t media;
  if (!parse_answer_media(sdp, sdp_len, &media) || !media.screen_section_present) {
    return false;
  }
  *screen_ssrc = media.screen_ssrc;
  *screen_rtx_ssrc = media.screen_rtx_ssrc;
  *screen_pt = media.screen_pt;
  *screen_rtx_pt = media.screen_rtx_pt;
  *screen_codec = media.screen_codec;
  *mid_extmap_id = media.mid_recv_extmap_id;
  return true;
}

static void emit_hook_event(const char *event, int64_t user_id, uint64_t room_id) {
  if (!event || event[0] == '\0' || room_id == 0) {
    return;
  }

  char msg[384];
  int n = snprintf(msg, sizeof(msg), "{\"user_id\":\"%" PRId64 "\",\"room_id\":\"%" PRIu64 "\",\"name\":\"\",\"event\":\"%s\"}", user_id, room_id, event);
  if (n <= 0 || (size_t)n >= sizeof(msg)) {
    SFU_LOG_WARN("signaling: hook payload too large for event=%s", event);
    return;
  }

  if (dispatch_hook_event(msg, n)) {
    SFU_LOG_INFO("signaling: hook event=%s user_id=%" PRId64 " room_id=%" PRIu64, event, user_id, room_id);
  } else {
    SFU_LOG_DEBUG("signaling: hook event=%s not published (nats unavailable)", event);
  }
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

static void disconnect_client(sfu_client_conn_t *c);

static void finish_client_close(sfu_client_conn_t *c) {
  SFU_LOG_INFO("signaling: client closed fd=%d ufrag=%s", c->fd, c->client_ufrag);

  const uint64_t room_id = c->joined_room_id;
  const int64_t user_id = c->user_id;
  const bool was_in_room = c->joined_room != NULL || room_id != 0;

  sfu_routing_table_unregister_fd(c->server->routing_table, c->fd);

  sfu_peer_session_t *session = NULL;
  if (c->client_ufrag[0] != '\0') {
    session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  }

  if (session) {
    if (session->fd != c->fd) {
      SFU_LOG_WARN("signaling: close fd=%d does not own session ufrag=%s fd=%d; leaving session alive", c->fd, c->client_ufrag, session->fd);
      sfu_session_release(session);
      session = NULL;
    }
  }

  if (session) {
    sfu_room_t *room = (sfu_room_t *)session->room;
    if (room) {
      notify_peer_left(room, session);
    }
    (void)sfu_session_begin_close(c->server->sessions, session);
    sfu_session_release(session);

    if (room) {
      sfu_signaling_trigger_renegotiation(room);
    }
  }

  if (was_in_room) {
    emit_hook_event("leave", user_id, room_id);
  }

  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  SFU_FREE(c);
}

static void on_client_handle_closed(uv_handle_t *handle) {
  sfu_client_conn_t *c = (sfu_client_conn_t *)handle->data;
  if (!c) {
    return;
  }
  if (c->handles_open > 0) {
    c->handles_open--;
  }
  if (c->handles_open == 0) {
    finish_client_close(c);
  }
}

static void on_keepalive_timer(uv_timer_t *timer) {
  sfu_client_conn_t *c = (sfu_client_conn_t *)timer->data;
  if (!c || c->disconnecting || !c->handshake_done) {
    return;
  }

  const uint64_t now_ms = sfu_now_ms();
  if (c->last_activity_ms != 0 && now_ms >= c->last_activity_ms && (now_ms - c->last_activity_ms) >= SFU_SIGNALING_IDLE_TIMEOUT_MS) {
    SFU_LOG_WARN("signaling: idle timeout (%u ms) fd=%d ufrag=%s; closing peer/session", SFU_SIGNALING_IDLE_TIMEOUT_MS, c->fd, c->client_ufrag);
    disconnect_client(c);
    return;
  }

  static const char ping_msg[] = "{\"type\":\"ping\"}";
  if (sfu_ws_send_text(c->fd, ping_msg, sizeof(ping_msg) - 1) != 0) {
    SFU_LOG_WARN("signaling: failed to send ping fd=%d; closing", c->fd);
    disconnect_client(c);
  }
}

static void mark_client_activity(sfu_client_conn_t *c) {
  if (c) {
    c->last_activity_ms = sfu_now_ms();
  }
}

static void start_client_keepalive(sfu_client_conn_t *c) {
  if (!c || !c->keepalive_inited || c->disconnecting) {
    return;
  }
  mark_client_activity(c);
  int rc = uv_timer_start(&c->keepalive_timer, on_keepalive_timer, SFU_SIGNALING_PING_INTERVAL_MS, SFU_SIGNALING_PING_INTERVAL_MS);
  if (rc != 0) {
    SFU_LOG_WARN("signaling: keepalive timer start failed fd=%d: %s", c->fd, uv_strerror(rc));
  }
}

static void disconnect_client(sfu_client_conn_t *c) {
  if (!c || c->disconnecting) {
    return;
  }
  c->disconnecting = true;

  if (c->keepalive_inited) {
    uv_timer_stop(&c->keepalive_timer);
    if (!uv_is_closing((uv_handle_t *)&c->keepalive_timer)) {
      uv_close((uv_handle_t *)&c->keepalive_timer, on_client_handle_closed);
    }
  }

  if (!uv_is_closing((uv_handle_t *)&c->poll_handle)) {
    uv_poll_stop(&c->poll_handle);
    uv_close((uv_handle_t *)&c->poll_handle, on_client_handle_closed);
  }
}

static void handle_ping(sfu_client_conn_t *c) {
  static const char pong_msg[] = "{\"type\":\"pong\"}";
  if (sfu_ws_send_text(c->fd, pong_msg, sizeof(pong_msg) - 1) != 0) {
    SFU_LOG_WARN("signaling: failed to send pong fd=%d; closing", c->fd);
    disconnect_client(c);
  }
}

static void handle_pong(sfu_client_conn_t *c) {
  /* Activity already recorded by the caller; keepalive only. */
  (void)c;
}

static void handle_join(sfu_client_conn_t *c, sfu_signaling_server_t *s, const char *buf, size_t n) {
  char role_str[16] = {0};
  char token[4096];
  uint64_t room_id = 0;
  int64_t user_id = 0;

  const char *jwt_secret = g_sfu_config.jwt_secret;
  if (jwt_secret[0] == '\0') {
    SFU_LOG_ERROR("signaling: jwt_secret is empty; rejecting join (fd=%d)", c->fd);
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"auth_not_configured\"}", 48);
    disconnect_client(c);
    return;
  }

  int token_len = sfu_json_extract_string(buf, n, "token", token, sizeof(token));
  if (token_len < 0) {
    SFU_LOG_WARN("signaling: join missing token (fd=%d)", c->fd);
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"missing_token\"}", 42);
    disconnect_client(c);
    return;
  }
  uint64_t token_room = 0;
  if (sfu_handshake_verify_join_token(token, (size_t)token_len, jwt_secret, &user_id, &token_room) != 0) {
    SFU_LOG_WARN("signaling: join JWT invalid (fd=%d)", c->fd);
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"invalid_token\"}", 42);
    disconnect_client(c);
    return;
  }
  room_id = token_room;
  c->user_id = user_id;
  SFU_LOG_INFO("signaling: join JWT ok user_id=%" PRId64 " room=%" PRIu64 " (fd=%d)", user_id, room_id, c->fd);

  c->is_audience = false;
  if (sfu_json_extract_string(buf, n, "role", role_str, sizeof(role_str)) >= 0) {
    if (strcmp(role_str, "audience") == 0) {
      c->is_audience = true;
    }
  }

  if (room_id == 0) {
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"invalid_room\"}", 41);
    return;
  }

  sfu_room_t *room = sfu_room_registry_get_or_create(s->room_registry, room_id);
  if (!room) {
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"room_creation_failed\"}", 49);
    return;
  }

  c->joined_room = room;
  c->joined_room_id = room_id;
  emit_hook_event("join", c->user_id, room_id);
  if (!c->is_audience) {
    emit_hook_event("publish", c->user_id, room_id);
  }

  SFU_LOG_INFO("signaling: peer %s joined room_id=%" PRIu64 " room=%p fd=%d", c->peer_ip, room_id, (void *)room, c->fd);

  if (!build_and_send_joined_response(c, room_id)) {
    SFU_LOG_WARN("signaling: failed to send joined response (fd=%d)", c->fd);
  }

  if (!build_and_send_initial_offer(c->fd, c->is_audience, s)) {
    SFU_LOG_WARN("signaling: failed to send initial offer (fd=%d)", c->fd);
  }
}

static void handle_answer(sfu_client_conn_t *c, sfu_signaling_server_t *s, const char *buf, size_t n) {
  if (!c->joined_room) {
    SFU_LOG_WARN("signaling: answer received before join completed for peer %s", c->peer_ip);
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"must_join_room_first\"}", 49);
    return;
  }

  char *sdp = s->scratch.sdp;
  int sdp_len = sfu_json_extract_string(buf, n, "sdp", sdp, SFU_SIGNALING_SDP_CAP);
  if (sdp_len < 0) {
    return;
  }

  char answer_ufrag[sizeof(c->client_ufrag)] = {0};
  bool have_ufrag = extract_sdp_ice_ufrag(sdp, (size_t)sdp_len, answer_ufrag, sizeof(answer_ufrag));
  if (have_ufrag && c->client_ufrag[0] != '\0' && strcmp(c->client_ufrag, answer_ufrag) != 0) {
    SFU_LOG_WARN("signaling: answer ufrag changed for fd=%d (%s -> %s), rejecting", c->fd, c->client_ufrag, answer_ufrag);
    static const char invalid_answer[] = "{\"type\":\"error\",\"message\":\"invalid_answer_ufrag\"}";
    sfu_ws_send_text(c->fd, invalid_answer, sizeof(invalid_answer) - 1);
    return;
  }
  if (have_ufrag && c->client_ufrag[0] == '\0') {
    snprintf(c->client_ufrag, sizeof(c->client_ufrag), "%s", answer_ufrag);
  }
  if (!have_ufrag && c->client_ufrag[0] == '\0') {
    SFU_LOG_WARN("signaling: initial answer has no ICE ufrag (fd=%d), rejecting", c->fd);
    static const char invalid_answer[] = "{\"type\":\"error\",\"message\":\"invalid_answer_ufrag\"}";
    sfu_ws_send_text(c->fd, invalid_answer, sizeof(invalid_answer) - 1);
    return;
  }

  sfu_answer_media_t media;
  if (!parse_answer_media(sdp, (size_t)sdp_len, &media)) {
    static const char invalid_answer[] = "{\"type\":\"error\",\"message\":\"invalid_answer_sdp\"}";
    sfu_ws_send_text(c->fd, invalid_answer, sizeof(invalid_answer) - 1);
    return;
  }

  sfu_pending_answer_t pending;
  memset(&pending, 0, sizeof(pending));
  pending.audio_ssrc = media.audio_ssrc;
  pending.video_ssrc = media.video_ssrc;
  pending.rtx_ssrc = media.rtx_ssrc;
  pending.screen_ssrc = media.screen_ssrc;
  pending.screen_rtx_ssrc = media.screen_rtx_ssrc;
  pending.video_pt = media.video_pt;
  pending.rtx_pt = media.rtx_pt;
  pending.video_codec = (uint8_t)media.video_codec;
  pending.screen_pt = media.screen_pt;
  pending.screen_rtx_pt = media.screen_rtx_pt;
  pending.screen_codec = (uint8_t)media.screen_codec;
  pending.twcc_recv_extmap_id = media.twcc_recv_extmap_id;
  pending.twcc_send_extmap_id = media.twcc_send_extmap_id;
  pending.mid_recv_extmap_id = media.mid_recv_extmap_id;
  pending.audio_section_present = media.audio_section_present;
  pending.video_section_present = media.video_section_present;
  pending.screen_section_present = media.screen_section_present;
  pending.audio_sends = media.audio_sends;
  pending.video_sends = media.video_sends;
  pending.screen_sends = media.screen_sends;
  pending.peer_id = generate_unique_id();
  pending.user_id = c->user_id;
  pending.is_audience = c->is_audience;
  pending.valid = true;

  uint32_t answer_generation = 0;
  sfu_routing_register_result_t register_result =
      sfu_routing_table_register_answer(s->routing_table, c->client_ufrag, c->joined_room, c->fd, &pending, &answer_generation);
  if (register_result != SFU_ROUTING_REGISTER_OK) {
    const char *message = register_result == SFU_ROUTING_REGISTER_OWNERSHIP_CONFLICT ? "duplicate_ice_ufrag"
                          : register_result == SFU_ROUTING_REGISTER_TABLE_FULL       ? "routing_table_full"
                                                                                     : "routing_registration_failed";
    char response[96];
    int response_len = snprintf(response, sizeof(response), "{\"type\":\"error\",\"message\":\"%s\"}", message);
    if (response_len > 0 && (size_t)response_len < sizeof(response)) {
      sfu_ws_send_text(c->fd, response, (size_t)response_len);
    }
    SFU_LOG_WARN("signaling: route registration failed for ufrag=%s fd=%d room_id=%" PRIu64 " outcome=%d", c->client_ufrag, c->fd, c->joined_room_id,
                 register_result);
    return;
  }
  SFU_LOG_INFO("signaling: atomically registered answer ufrag=%s -> room_id=%" PRIu64 " generation=%u", c->client_ufrag, c->joined_room_id, answer_generation);

  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(s->sessions, c->client_ufrag);
  if (!session) {
    SFU_LOG_INFO("signaling: answer for ufrag=%s awaits authenticated STUN bind", c->client_ufrag);
    return;
  }

  bool follow_up_pending = false;
  uint32_t answered_offer_generation = 0;
  pthread_mutex_lock(&session->negotiation_lock);
  if (session->offer_outstanding) {
    session->offer_outstanding = false;
    answered_offer_generation = session->offer_generation;
  }
  follow_up_pending = session->renegotiation_pending;
  pthread_mutex_unlock(&session->negotiation_lock);
  if (answered_offer_generation != 0) {
    SFU_LOG_INFO("signaling: completed renegotiation answer ufrag=%s peer_id=%u generation=%u pending=%d", c->client_ufrag, session->peer_id,
                 answered_offer_generation, follow_up_pending);
  }

  bool role_changed = false;
  bool media_changed = false;
  bool newly_bound = false;
  if (sfu_routing_table_reconcile_answer(s->routing_table, c->client_ufrag, c->joined_room, c->fd, answer_generation, session, &role_changed, &media_changed)) {
    if (!session->room) {
      room_add_peer(c->joined_room, session);
      newly_bound = session->room == c->joined_room;
      if (newly_bound) {
        sfu_signaling_notify_peer_admitted(c->joined_room, session);
      }
    }
    if (session->room && (media_changed || role_changed || newly_bound)) {
      SFU_LOG_INFO("answer: media/role changed for ufrag=%s generation=%u (media=%d role=%d bound=%d), refreshing%s", c->client_ufrag, answer_generation,
                   media_changed, role_changed, newly_bound, answered_offer_generation == 0 ? " + renegotiating" : "");
      room_refresh_peer_streams((sfu_room_t *)session->room, session);
      if (answered_offer_generation == 0 || role_changed || newly_bound) {
        sfu_signaling_trigger_renegotiation((sfu_room_t *)session->room);
      }
    }
  }
  if (follow_up_pending && session->room) {
    sfu_signaling_trigger_renegotiation((sfu_room_t *)session->room);
  }
  sfu_session_release(session);
}

static void broadcast_peer_updated(sfu_room_t *room, sfu_peer_session_t *session) {
  if (!room || !session) {
    return;
  }
  int fds[SFU_ROOM_MAX_PEERS];
  uint32_t count = 0;
  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->peer_count && count < SFU_ROOM_MAX_PEERS; i++) {
    sfu_peer_session_t *peer = room->peers[i];
    if (peer && peer->fd >= 0 && sfu_session_accepts_work(peer)) {
      fds[count++] = peer->fd;
    }
  }
  pthread_mutex_unlock(&room->lock);

  bool is_audience = atomic_load_explicit(&session->is_audience, memory_order_acquire);
  bool is_mute = atomic_load_explicit(&session->is_mute, memory_order_acquire);
  char event[288];
  int n = snprintf(event, sizeof(event), "{\"type\":\"peer_updated\",\"peer\":{\"peer_id\":%u,\"user_id\":\"%" PRId64 "\",\"role\":\"%s\",\"is_mute\":%s}}",
                   session->peer_id, session->user_id, is_audience ? "audience" : "speaker", is_mute ? "true" : "false");
  if (n > 0 && (size_t)n < sizeof(event)) {
    for (uint32_t i = 0; i < count; i++) {
      (void)sfu_ws_send_text(fds[i], event, (size_t)n);
    }
  }
}

static void handle_push_to_talk(sfu_client_conn_t *c, sfu_signaling_server_t *s, const char *buf, size_t n, bool push_to_talk) {
  (void)s;
  char role_str[16] = {0};
  if (push_to_talk) {
    snprintf(role_str, sizeof(role_str), "speaker");
  }
  if (!c->joined_room || c->client_ufrag[0] == '\0' || (!push_to_talk && sfu_json_extract_string(buf, n, "role", role_str, sizeof(role_str)) < 0) ||
      (strcmp(role_str, "speaker") != 0 && strcmp(role_str, "audience") != 0)) {
    static const char invalid_role_change[] = "{\"type\":\"error\",\"message\":\"invalid_role_change\"}";
    sfu_ws_send_text(c->fd, invalid_role_change, sizeof(invalid_role_change) - 1);
    return;
  }

  bool is_audience = strcmp(role_str, "audience") == 0;
  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  bool changed = false;
  if (session && session->room == c->joined_room) {
    pthread_mutex_lock(&session->answer_lock);
    uint32_t invalidated_generation = 0;
    if (sfu_routing_table_invalidate_pending(c->server->routing_table, c->client_ufrag, c->joined_room, c->fd, &invalidated_generation)) {
      atomic_store_explicit(&session->applied_answer_generation, invalidated_generation, memory_order_release);
    }
    changed = room_update_peer_role(c->joined_room, session, is_audience);
    pthread_mutex_unlock(&session->answer_lock);
  }
  if (!changed) {
    static const char role_change_rejected[] = "{\"type\":\"error\",\"message\":\"role_change_rejected\"}";
    sfu_ws_send_text(c->fd, role_change_rejected, sizeof(role_change_rejected) - 1);
  } else {
    c->is_audience = is_audience;
    char response[128];
    int response_len = snprintf(response, sizeof(response), "{\"type\":\"role_changed\",\"user_id\":\"%" PRIu64 "\",\"role\":\"%s\"}", c->user_id, role_str);
    if (response_len > 0 && (size_t)response_len < sizeof(response)) {
      sfu_ws_send_text(c->fd, response, (size_t)response_len);
    }
    broadcast_peer_updated(c->joined_room, session);
    sfu_signaling_trigger_renegotiation(c->joined_room);
    emit_hook_event(is_audience ? "unpublish" : "publish", c->user_id, c->joined_room_id);
  }
  if (session) {
    sfu_session_release(session);
  }
}

static void handle_camera(sfu_client_conn_t *c, const char *buf, size_t n) {
  if (!c->joined_room || c->client_ufrag[0] == '\0' || c->is_audience) {
    return;
  }
  bool active = true;
  (void)sfu_json_extract_bool(buf, n, "active", &active);
  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  if (!session || session->room != c->joined_room) {
    if (session) {
      sfu_session_release(session);
    }
    return;
  }

  pthread_mutex_lock(&session->media_lock);
  bool camera_active = active && atomic_load_explicit(&session->video_send_negotiated, memory_order_acquire) && session->uplink_video.ssrc != 0;
  bool changed = session->uplink_video.active != camera_active;
  session->uplink_video.active = camera_active;
  sfu_session_publish_media(session);
  pthread_mutex_unlock(&session->media_lock);

  if (changed) {
    room_refresh_peer_streams(c->joined_room, session);
    broadcast_peer_updated(c->joined_room, session);
    sfu_signaling_trigger_renegotiation(c->joined_room);
  }
  emit_hook_event(active ? "publish" : "unpublish", c->user_id, c->joined_room_id);
  sfu_session_release(session);
}

static void handle_screen_share(sfu_client_conn_t *c, const char *buf, size_t n) {
  if (!c->joined_room || c->client_ufrag[0] == '\0' || c->is_audience) {
    static const char unavailable[] = "{\"type\":\"error\",\"message\":\"screen_share_not_allowed\"}";
    sfu_ws_send_text(c->fd, unavailable, sizeof(unavailable) - 1);
    return;
  }
  bool active = true;
  (void)sfu_json_extract_bool(buf, n, "active", &active);
  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  if (!session || session->room != c->joined_room) {
    static const char no_session[] = "{\"type\":\"error\",\"message\":\"session_not_found\"}";
    sfu_ws_send_text(c->fd, no_session, sizeof(no_session) - 1);
    if (session) {
      sfu_session_release(session);
    }
    return;
  }
  pthread_mutex_lock(&session->media_lock);
  session->screen.active = active && atomic_load_explicit(&session->screen_send_negotiated, memory_order_acquire);
  sfu_session_publish_media(session);
  pthread_mutex_unlock(&session->media_lock);
  room_refresh_peer_streams(c->joined_room, session);
  sfu_signaling_trigger_renegotiation(c->joined_room);
  sfu_session_release(session);
  emit_hook_event(active ? "share_screen" : "unshare_screen", c->user_id, c->joined_room_id);
  char response[80];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"screen_share_changed\",\"active\":%s}", active ? "true" : "false");
  if (response_len > 0 && (size_t)response_len < sizeof(response)) {
    sfu_ws_send_text(c->fd, response, (size_t)response_len);
  }
}

static void handle_visibility(sfu_client_conn_t *c, const char *buf, size_t n) {
  if (!c->joined_room || c->client_ufrag[0] == '\0') {
    static const char must_join[] = "{\"type\":\"error\",\"message\":\"must_join_room_first\"}";
    sfu_ws_send_text(c->fd, must_join, sizeof(must_join) - 1);
    return;
  }

  bool visible = true;
  if (sfu_json_extract_bool(buf, n, "visible", &visible) != 0) {
    static const char invalid_visibility[] = "{\"type\":\"error\",\"message\":\"invalid_visibility\"}";
    sfu_ws_send_text(c->fd, invalid_visibility, sizeof(invalid_visibility) - 1);
    return;
  }

  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  if (!session || session->room != c->joined_room) {
    static const char no_session[] = "{\"type\":\"error\",\"message\":\"session_not_found\"}";
    sfu_ws_send_text(c->fd, no_session, sizeof(no_session) - 1);
    if (session) {
      sfu_session_release(session);
    }
    return;
  }

  atomic_store_explicit(&session->visible, visible, memory_order_release);
  sfu_session_release(session);

  char response[96];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"visibility_changed\",\"visible\":%s}", visible ? "true" : "false");
  if (response_len > 0 && (size_t)response_len < sizeof(response)) {
    sfu_ws_send_text(c->fd, response, (size_t)response_len);
  }
  SFU_LOG_INFO("signaling: visibility user_id=%" PRId64 " visible=%s (fd=%d)", c->user_id, visible ? "true" : "false", c->fd);
}

static void handle_mute(sfu_client_conn_t *c, const char *buf, size_t n) {
  if (!c->joined_room || c->client_ufrag[0] == '\0') {
    static const char must_join[] = "{\"type\":\"error\",\"message\":\"must_join_room_first\"}";
    sfu_ws_send_text(c->fd, must_join, sizeof(must_join) - 1);
    return;
  }

  bool is_mute = false;
  if (sfu_json_extract_bool(buf, n, "is_mute", &is_mute) != 0) {
    static const char invalid_mute[] = "{\"type\":\"error\",\"message\":\"invalid_mute\"}";
    sfu_ws_send_text(c->fd, invalid_mute, sizeof(invalid_mute) - 1);
    return;
  }

  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  if (!session || session->room != c->joined_room) {
    static const char no_session[] = "{\"type\":\"error\",\"message\":\"session_not_found\"}";
    sfu_ws_send_text(c->fd, no_session, sizeof(no_session) - 1);
    if (session) {
      sfu_session_release(session);
    }
    return;
  }

  atomic_store_explicit(&session->is_mute, is_mute, memory_order_release);
  broadcast_peer_updated(c->joined_room, session);
  sfu_session_release(session);

  char response[80];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"mute_changed\",\"is_mute\":%s}", is_mute ? "true" : "false");
  if (response_len > 0 && (size_t)response_len < sizeof(response)) {
    sfu_ws_send_text(c->fd, response, (size_t)response_len);
  }
  SFU_LOG_INFO("signaling: mute user_id=%" PRId64 " is_mute=%s (fd=%d)", c->user_id, is_mute ? "true" : "false", c->fd);
}

static void dispatch_client_message(sfu_client_conn_t *c, sfu_signaling_server_t *s, const char *buf, size_t n) {
  char type[32];
  if (sfu_json_extract_string(buf, n, "type", type, sizeof(type)) < 0) {
    return;
  }

  if (strcmp(type, "ping") == 0) {
    handle_ping(c);
  } else if (strcmp(type, "pong") == 0) {
    handle_pong(c);
  } else if (strcmp(type, "join") == 0) {
    handle_join(c, s, buf, n);
  } else if (strcmp(type, "answer") == 0) {
    handle_answer(c, s, buf, n);
  } else if (strcmp(type, "push_to_talk") == 0 || strcmp(type, "role_change") == 0) {
    handle_push_to_talk(c, s, buf, n, strcmp(type, "push_to_talk") == 0);
  } else if (strcmp(type, "visibility") == 0) {
    handle_visibility(c, buf, n);
  } else if (strcmp(type, "mute") == 0) {
    handle_mute(c, buf, n);
  } else if (strcmp(type, "camera") == 0) {
    handle_camera(c, buf, n);
  } else if (strcmp(type, "share_screen") == 0) {
    handle_screen_share(c, buf, n);
  } else {
    SFU_LOG_DEBUG("signaling: unrecognized message type \"%s\" from peer %s", type, c->peer_ip);
  }
}

static void on_client_readable(uv_poll_t *handle, int status, int events) {
  sfu_client_conn_t *c = (sfu_client_conn_t *)handle->data;
  sfu_signaling_server_t *s = c->server;

  if (status < 0 || (events & UV_DISCONNECT)) {
    disconnect_client(c);
    return;
  }

  if (!(events & UV_READABLE)) {
    return;
  }

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
      return;
    }

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
    start_client_keepalive(c);
    return;
  }

  char *buf = s->scratch.recv;
  ssize_t n = sfu_ws_recv_text(c->fd, buf, SFU_SIGNALING_RECV_CAP);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    disconnect_client(c);
    return;
  }
  if (n == 0) {
    disconnect_client(c);
    return;
  }

  mark_client_activity(c);
  dispatch_client_message(c, s, buf, (size_t)n);
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

      sfu_client_conn_t *c = (sfu_client_conn_t *)SFU_CALLOC(1, sizeof(sfu_client_conn_t));
      if (!c) {
        close(fd);
        return;
      }
      c->fd = fd;
      c->server = s;
      c->handshake_done = false;
      c->disconnecting = false;
      c->keepalive_inited = false;
      c->handles_open = 0;
      c->last_activity_ms = 0;
      strcpy(c->peer_ip, "unknown");

      int rc = uv_poll_init_socket(handle->loop, &c->poll_handle, fd);
      if (rc != 0) {
        SFU_LOG_ERROR("signaling: uv_poll_init_socket failed: %s", uv_strerror(rc));
        close(fd);
        SFU_FREE(c);
        return;
      }
      c->poll_handle.data = c;
      c->handles_open = 1;

      rc = uv_timer_init(handle->loop, &c->keepalive_timer);
      if (rc != 0) {
        SFU_LOG_ERROR("signaling: uv_timer_init failed: %s", uv_strerror(rc));
        c->disconnecting = true;
        uv_close((uv_handle_t *)&c->poll_handle, on_client_handle_closed);
        return;
      }
      c->keepalive_timer.data = c;
      c->keepalive_inited = true;
      c->handles_open = 2;

      rc = uv_poll_start(&c->poll_handle, UV_READABLE, on_client_readable);
      if (rc != 0) {
        SFU_LOG_ERROR("signaling: uv_poll_start failed: %s", uv_strerror(rc));
        disconnect_client(c);
      }
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      SFU_LOG_ERROR("signaling: accept failed: %s", strerror(errno));
    }
  }
}

static sfu_peer_session_t *membership_queue_pop(sfu_membership_queue_t *queue) {
  pthread_mutex_lock(&queue->lock);
  sfu_peer_session_t *session = NULL;
  if (queue->count > 0) {
    session = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % SFU_MEMBERSHIP_QUEUE_CAP;
    queue->count--;
  }
  pthread_mutex_unlock(&queue->lock);
  return session;
}

typedef struct {
  sfu_peer_session_t *session;
  uint32_t peer_id;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint32_t mid_screen;
  int64_t user_id;
  int fd;
  bool is_audience;
  bool is_mute;
  char ufrag[32];
} membership_view_t;

static void membership_find_mids(sfu_peer_session_t *receiver, sfu_peer_session_t *source, uint32_t *mid_audio, uint32_t *mid_video, uint32_t *mid_screen) {
  *mid_audio = 0;
  *mid_video = 0;
  *mid_screen = 0;
  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(receiver);
  if (!snap) {
    return;
  }
  for (uint32_t i = 0; i < snap->count; i++) {
    if (snap->entries[i].subscriber == source) {
      *mid_audio = snap->entries[i].mid_audio;
      *mid_video = snap->entries[i].mid_video;
      *mid_screen = snap->entries[i].mid_screen;
      break;
    }
  }
  sfu_subscriptions_snapshot_release(snap);
}

static void flush_membership_events(sfu_signaling_server_t *s) {
  sfu_peer_session_t *joined;
  while ((joined = membership_queue_pop(&s->membership_queue)) != NULL) {
    sfu_room_t *room = (sfu_room_t *)joined->room;
    membership_view_t members[SFU_ROOM_MAX_PEERS];
    uint32_t count = 0;
    uint64_t room_id = 0;

    if (room) {
      pthread_mutex_lock(&room->lock);
      room_id = room->room_id;
      for (uint32_t i = 0; i < room->peer_count && count < SFU_ROOM_MAX_PEERS; i++) {
        sfu_peer_session_t *peer = room->peers[i];
        if (!peer || !sfu_session_accepts_work(peer) || peer->fd < 0) {
          continue;
        }
        atomic_fetch_add_explicit(&peer->refcount, 1, memory_order_relaxed);
        members[count] = (membership_view_t){
            .session = peer,
            .peer_id = peer->peer_id,
            .user_id = peer->user_id,
            .fd = peer->fd,
            .is_audience = atomic_load_explicit(&peer->is_audience, memory_order_acquire),
            .is_mute = atomic_load_explicit(&peer->is_mute, memory_order_acquire),
        };
        if (peer->cold) {
          snprintf(members[count].ufrag, sizeof(members[count].ufrag), "%s", peer->cold->ufrag);
        }
        count++;
      }
      pthread_mutex_unlock(&room->lock);
      for (uint32_t i = 0; i < count; i++) {
        if (members[i].session != joined) {
          membership_find_mids(joined, members[i].session, &members[i].mid_audio, &members[i].mid_video, &members[i].mid_screen);
        }
      }
    }

    if (room && joined->fd >= 0) {
      char *snapshot = s->scratch.json;
      size_t off = 0;
      int n = snprintf(snapshot, SFU_SIGNALING_JSON_CAP,
                       "{\"type\":\"room_snapshot\",\"room\":\"%" PRIu64 "\",\"self_peer_id\":%u,\"participant_count\":%u,\"members\":[", room_id,
                       joined->peer_id, count);
      if (n > 0 && (size_t)n < SFU_SIGNALING_JSON_CAP) {
        off = (size_t)n;
        for (uint32_t i = 0; i < count; i++) {
          n = snprintf(snapshot + off, SFU_SIGNALING_JSON_CAP - off,
                       "%s{\"peer_id\":%u,\"user_id\":\"%" PRId64
                       "\",\"role\":\"%s\",\"is_mute\":%s,\"ufrag\":\"%s\",\"mid_audio\":%u,\"mid_video\":%u,\"mid_screen\":%u}",
                       i ? "," : "", members[i].peer_id, members[i].user_id, members[i].is_audience ? "audience" : "speaker",
                       members[i].is_mute ? "true" : "false", members[i].ufrag, members[i].mid_audio, members[i].mid_video, members[i].mid_screen);
          if (n < 0 || (size_t)n >= SFU_SIGNALING_JSON_CAP - off) {
            off = 0;
            break;
          }
          off += (size_t)n;
        }
        if (off > 0 && off + 2 < SFU_SIGNALING_JSON_CAP) {
          snapshot[off++] = ']';
          snapshot[off++] = '}';
          snapshot[off] = '\0';
          (void)sfu_ws_send_text(joined->fd, snapshot, off);
        }
      }

      bool joined_audience = atomic_load_explicit(&joined->is_audience, memory_order_acquire);
      for (uint32_t i = 0; i < count; i++) {
        if (members[i].peer_id == joined->peer_id) {
          continue;
        }
        uint32_t mid_audio = 0;
        uint32_t mid_video = 0;
        uint32_t mid_screen = 0;
        membership_find_mids(members[i].session, joined, &mid_audio, &mid_video, &mid_screen);
        bool joined_mute = atomic_load_explicit(&joined->is_mute, memory_order_acquire);
        char event[416];
        int event_len = snprintf(event, sizeof(event),
                                 "{\"type\":\"peer_joined\",\"participant_count\":%u,\"peer\":{\"peer_id\":%u,\"user_id\":\"%" PRId64
                                 "\",\"role\":\"%s\",\"is_mute\":%s,\"ufrag\":\"%s\",\"mid_audio\":%u,\"mid_video\":%u,\"mid_screen\":%u}}",
                                 count, joined->peer_id, joined->user_id, joined_audience ? "audience" : "speaker", joined_mute ? "true" : "false",
                                 joined->cold ? joined->cold->ufrag : "", mid_audio, mid_video, mid_screen);
        if (event_len > 0 && (size_t)event_len < sizeof(event)) {
          (void)sfu_ws_send_text(members[i].fd, event, (size_t)event_len);
        }
      }
    }
    for (uint32_t i = 0; i < count; i++) {
      sfu_session_release(members[i].session);
    }
    sfu_session_release(joined);
  }
}

static sfu_peer_session_t *renegotiation_queue_pop(sfu_renegotiation_queue_t *queue) {
  pthread_mutex_lock(&queue->lock);
  sfu_peer_session_t *session = NULL;
  if (queue->count > 0) {
    session = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % SFU_RENEGOTIATION_QUEUE_CAP;
    queue->count--;
  }
  pthread_mutex_unlock(&queue->lock);
  return session;
}

static void flush_pending_offers(sfu_signaling_server_t *s) {
  sfu_peer_session_t *session;
  while ((session = renegotiation_queue_pop(&s->renegotiation_queue)) != NULL) {
    int fd = -1;
    uint32_t generation = 0;
    bool send = false;
    pthread_mutex_lock(&session->negotiation_lock);
    session->negotiation_needed = false;
    if (session->state != SFU_SESSION_ESTABLISHED) {
      session->renegotiation_pending = true;
      pthread_mutex_unlock(&session->negotiation_lock);
      sfu_session_release(session);
      continue;
    }
    if (session->renegotiation_pending && !session->offer_outstanding && sfu_session_accepts_work(session) && session->fd >= 0) {
      session->renegotiation_pending = false;
      session->offer_outstanding = true;
      session->offer_generation++;
      generation = session->offer_generation;
      fd = session->fd;
      send = true;
    }
    pthread_mutex_unlock(&session->negotiation_lock);

    if (send && build_and_send_offer(fd, session, s)) {
      SFU_LOG_INFO("signaling: sent renegotiation offer ufrag=%s fd=%d peer_id=%u generation=%u", session->cold->ufrag, fd, session->peer_id, generation);
    } else if (send) {
      pthread_mutex_lock(&session->negotiation_lock);
      if (session->offer_generation == generation) {
        session->offer_outstanding = false;
        session->renegotiation_pending = true;
        session->negotiation_needed = true;
      }
      pthread_mutex_unlock(&session->negotiation_lock);
      SFU_LOG_WARN("signaling: failed renegotiation offer ufrag=%s fd=%d peer_id=%u generation=%u", session->cold->ufrag, fd, session->peer_id, generation);
    }
    sfu_session_release(session);
  }
}

static void on_renegotiation_wake(uv_async_t *handle) {
  sfu_signaling_server_t *s = handle->data;
  if (s && atomic_load(&s->running)) {
    flush_membership_events(s);
    flush_pending_offers(s);
  }
}

static void on_async_wake(uv_async_t *handle) { uv_stop(handle->loop); }

static void on_shutdown_walk(uv_handle_t *handle, void *arg) {
  if (uv_is_closing(handle)) {
    return;
  }

  sfu_signaling_server_t *s = (sfu_signaling_server_t *)arg;

  if (handle->type == UV_POLL && handle->data != NULL && handle->data != s) {
    disconnect_client((sfu_client_conn_t *)handle->data);
    return;
  }
  if (handle->type == UV_TIMER && handle->data != NULL) {
    sfu_client_conn_t *c = (sfu_client_conn_t *)handle->data;
    if (c->server == s || c->server != NULL) {
      disconnect_client(c);
      return;
    }
  }
  uv_close(handle, NULL);
}

static void *signaling_loop_main(void *arg) {
  sfu_signaling_server_t *s = (sfu_signaling_server_t *)arg;

  uv_loop_t loop;
  uv_loop_init(&loop);

  uv_async_init(&loop, &s->async_waker, on_async_wake);
  s->async_waker.data = s;
  uv_async_init(&loop, &s->renegotiation_waker, on_renegotiation_wake);
  s->renegotiation_waker.data = s;

  uv_poll_t listen_poll;
  uv_poll_init_socket(&loop, &listen_poll, s->listen_fd);
  listen_poll.data = s;
  uv_poll_start(&listen_poll, UV_READABLE, on_server_readable);

  while (atomic_load(&s->running)) {
    uv_run(&loop, UV_RUN_DEFAULT);
  }

  uv_walk(&loop, on_shutdown_walk, s);

  uv_run(&loop, UV_RUN_DEFAULT);

  uv_loop_close(&loop);
  return NULL;
}

static bool signaling_scratch_init(sfu_signaling_scratch_t *scratch) {
  scratch->recv = SFU_MALLOC(SFU_SIGNALING_RECV_CAP);
  scratch->sdp = SFU_MALLOC(SFU_SIGNALING_SDP_CAP);
  scratch->json = SFU_MALLOC(SFU_SIGNALING_JSON_CAP);
  if (scratch->recv && scratch->sdp && scratch->json) {
    return true;
  }
  SFU_FREE(scratch->json);
  SFU_FREE(scratch->sdp);
  SFU_FREE(scratch->recv);
  memset(scratch, 0, sizeof(*scratch));
  return false;
}

static void signaling_scratch_destroy(sfu_signaling_scratch_t *scratch) {
  SFU_FREE(scratch->json);
  SFU_FREE(scratch->sdp);
  SFU_FREE(scratch->recv);
  memset(scratch, 0, sizeof(*scratch));
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
  if (pthread_mutex_init(&s->renegotiation_queue.lock, NULL) != 0) {
    return -1;
  }
  if (pthread_mutex_init(&s->membership_queue.lock, NULL) != 0) {
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }
  if (!signaling_scratch_init(&s->scratch)) {
    SFU_LOG_ERROR("signaling: failed to allocate loop scratch workspace");
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }

  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->listen_fd < 0) {
    SFU_LOG_ERROR("signaling: socket() failed");
    signaling_scratch_destroy(&s->scratch);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
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
    signaling_scratch_destroy(&s->scratch);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }
  if (listen(s->listen_fd, 16) < 0) {
    SFU_LOG_ERROR("signaling: listen() failed");
    close(s->listen_fd);
    signaling_scratch_destroy(&s->scratch);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }

  atomic_store(&s->running, true);
  if (pthread_create(&s->thread, NULL, signaling_loop_main, s) != 0) {
    SFU_LOG_ERROR("signaling: failed to spawn accept loop thread");
    close(s->listen_fd);
    signaling_scratch_destroy(&s->scratch);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }

  g_signaling_server = s;

  SFU_LOG_INFO("signaling server listening on ws://0.0.0.0:%u", listen_port);
  return 0;
}

void sfu_signaling_server_stop(sfu_signaling_server_t *s) {
  if (!s || !atomic_load(&s->running)) {
    return;
  }

  atomic_store(&s->running, false);
  uv_async_send(&s->async_waker);

  pthread_join(s->thread, NULL);
  close(s->listen_fd);

  if (g_signaling_server == s) {
    g_signaling_server = NULL;
  }
  sfu_peer_session_t *queued;
  while ((queued = renegotiation_queue_pop(&s->renegotiation_queue)) != NULL) {
    sfu_session_release(queued);
  }
  while ((queued = membership_queue_pop(&s->membership_queue)) != NULL) {
    sfu_session_release(queued);
  }
  signaling_scratch_destroy(&s->scratch);
  pthread_mutex_destroy(&s->membership_queue.lock);
  pthread_mutex_destroy(&s->renegotiation_queue.lock);
}

void sfu_signaling_generate_turn_credentials(const char *secret, const char *username_suffix, char *out_username, size_t user_sz, char *out_password,
                                             size_t pass_sz, uint32_t ttl_seconds) {
  if (!secret || !out_username || !out_password || user_sz == 0 || pass_sz == 0) {
    return;
  }

  time_t expiry = time(NULL) + (ttl_seconds > 0 ? ttl_seconds : 86400);
  snprintf(out_username, user_sz, "%ld:%s", (long)expiry, username_suffix ? username_suffix : "user");

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;

  /* BoringSSL HMAC API */
  if (!HMAC(EVP_sha1(), secret, strlen(secret), (const unsigned char *)out_username, strlen(out_username), digest, &digest_len)) {
    out_password[0] = '\0';
    return;
  }

  /* BoringSSL EVP_EncodeBlock calculates required output length as 4 * ((in_len + 2) / 3) + 1 null byte */
  size_t required_b64_len = 4 * ((digest_len + 2) / 3) + 1;
  if (pass_sz < required_b64_len) {
    SFU_LOG_ERROR("signaling: pass_sz buffer too small for TURN password");
    out_password[0] = '\0';
    return;
  }

  /* EVP_EncodeBlock encodes to base64 and automatically null-terminates out_password */
  EVP_EncodeBlock((uint8_t *)out_password, digest, digest_len);
}
