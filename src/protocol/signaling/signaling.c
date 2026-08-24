#include "protocol/signaling/signaling.h"
#include <arpa/inet.h>
#include <assert.h>
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
#include "runtime/timer.h"
#include "util/alloc.h"
#include "util/log.h"

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
static pthread_mutex_t g_signaling_producer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_signaling_producer_idle = PTHREAD_COND_INITIALIZER;
static uint32_t g_signaling_producers;
static bool g_signaling_stopping;
static atomic_uint g_membership_test_fail_allocations;
static void broadcast_peer_updated(sfu_room_t *room, sfu_peer_session_t *session);
static sfu_peer_session_t *media_update_queue_pop(sfu_membership_queue_t *queue);

static sfu_signaling_server_t *signaling_producer_acquire(void) {
  pthread_mutex_lock(&g_signaling_producer_lock);
  sfu_signaling_server_t *server = (!g_signaling_stopping && g_signaling_server) ? g_signaling_server : NULL;
  if (server) {
    g_signaling_producers++;
  }
  pthread_mutex_unlock(&g_signaling_producer_lock);
  return server;
}

static void signaling_producer_release(void) {
  pthread_mutex_lock(&g_signaling_producer_lock);
  assert(g_signaling_producers > 0);
  if (--g_signaling_producers == 0) {
    pthread_cond_broadcast(&g_signaling_producer_idle);
  }
  pthread_mutex_unlock(&g_signaling_producer_lock);
}

void sfu_membership_event_test_fail_allocations(uint32_t count) { atomic_store_explicit(&g_membership_test_fail_allocations, count, memory_order_release); }

sfu_membership_event_t *sfu_membership_event_alloc(void) {
  uint32_t failures = atomic_load_explicit(&g_membership_test_fail_allocations, memory_order_acquire);
  while (failures > 0 &&
         !atomic_compare_exchange_weak_explicit(&g_membership_test_fail_allocations, &failures, failures - 1, memory_order_acq_rel, memory_order_acquire)) {
  }
  return failures > 0 ? NULL : SFU_CALLOC(1, sizeof(sfu_membership_event_t));
}

void sfu_membership_event_release(sfu_membership_event_t *event) {
  if (!event) {
    return;
  }
  for (uint32_t i = 0; i < event->recipient_count; i++) {
    if (event->recipients[i].session) {
      sfu_session_release(event->recipients[i].session);
    }
  }
  if (event->preallocated_storage) {
    sfu_peer_session_t *owner = event->storage_owner;
    memset(event, 0, sizeof(*event));
    event->preallocated_storage = true;
    event->storage_owner = owner;
    atomic_store_explicit(&owner->leave_event_in_use, false, memory_order_release);
    sfu_session_release(owner);
  } else {
    SFU_FREE(event);
  }
}

bool sfu_signaling_reserve_membership_event(sfu_membership_reservation_t *reservation) {
  if (!reservation) {
    return false;
  }
  reservation->server = NULL;
  sfu_signaling_server_t *server = signaling_producer_acquire();
  if (!server) {
    return false;
  }
  sfu_membership_queue_t *queue = &server->membership_queue;
  pthread_mutex_lock(&queue->lock);
  while (queue->accepting && queue->count + queue->reserved_count >= SFU_MEMBERSHIP_QUEUE_CAP) {
    pthread_cond_wait(&queue->not_full, &queue->lock);
  }
  if (!queue->accepting) {
    pthread_mutex_unlock(&queue->lock);
    signaling_producer_release();
    return false;
  }
  queue->reserved_count++;
  pthread_mutex_unlock(&queue->lock);
  reservation->server = server;
  return true;
}

void sfu_signaling_commit_membership_event(sfu_membership_reservation_t *reservation, sfu_membership_event_t *event) {
  assert(reservation && reservation->server && event);
  sfu_signaling_server_t *server = reservation->server;
  sfu_membership_queue_t *queue = &server->membership_queue;
  pthread_mutex_lock(&queue->lock);
  assert(queue->reserved_count > 0 && queue->count < SFU_MEMBERSHIP_QUEUE_CAP);
  queue->reserved_count--;
  if (server->test_auto_drain) {
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
    reservation->server = NULL;
    sfu_membership_event_release(event);
    signaling_producer_release();
    return;
  }
  queue->items[queue->tail] = event;
  queue->tail = (queue->tail + 1) % SFU_MEMBERSHIP_QUEUE_CAP;
  queue->count++;
  pthread_mutex_unlock(&queue->lock);
  reservation->server = NULL;
  if (!server->suppress_wake) {
    uv_async_send(&server->renegotiation_waker);
  }
  signaling_producer_release();
}

void sfu_signaling_cancel_membership_event(sfu_membership_reservation_t *reservation) {
  if (!reservation || !reservation->server) {
    return;
  }
  sfu_membership_queue_t *queue = &reservation->server->membership_queue;
  pthread_mutex_lock(&queue->lock);
  assert(queue->reserved_count > 0);
  queue->reserved_count--;
  pthread_cond_signal(&queue->not_full);
  pthread_mutex_unlock(&queue->lock);
  reservation->server = NULL;
  signaling_producer_release();
}

bool sfu_signaling_queue_membership_event(sfu_membership_event_t *event) {
  if (!event) {
    return false;
  }
  sfu_membership_reservation_t reservation;
  if (!sfu_signaling_reserve_membership_event(&reservation)) {
    sfu_membership_event_release(event);
    return false;
  }
  sfu_signaling_commit_membership_event(&reservation, event);
  return true;
}


static bool send_offer_json(int fd, sfu_signaling_server_t *s, size_t offer_len, uint64_t offer_generation) {
  char prefix[96];
  int prefix_written = snprintf(prefix, sizeof(prefix), "{\"type\":\"offer\",\"offer_generation\":%" PRIu64 ",\"sdp\":\"", offer_generation);
  if (prefix_written <= 0 || (size_t)prefix_written >= sizeof(prefix)) {
    return false;
  }
  size_t prefix_len = (size_t)prefix_written;
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
  if (!send_offer_json(fd, s, (size_t)offer_len, 0)) {
    SFU_LOG_WARN("signaling: failed to send initial offer (fd=%d)", fd);
    return false;
  }
  SFU_LOG_INFO("signaling: sent initial server offer (fd=%d)", fd);
  return true;
}

static bool build_and_send_offer(int fd, sfu_peer_session_t *session, sfu_signaling_server_t *s, uint64_t *offer_generation) {
  bool captured = false;
  sfu_remote_offer_manifest_t *manifest = sfu_session_remote_offer_acquire_current(session);
  if (!manifest) {
    manifest = sfu_session_remote_offer_capture(session);
    captured = true;
  }
  if (!manifest) {
    SFU_LOG_WARN("signaling: failed to capture immutable offer manifest (fd=%d)", fd);
    return false;
  }
  int offer_len = sfu_sdp_build_offer_manifest(session, manifest, s->media_host, s->media_port, s->ice_creds->ufrag, s->ice_creds->pwd,
                                               s->dtls_ctx->fingerprint, s->scratch.sdp, SFU_SIGNALING_SDP_CAP, NULL);
  if (offer_len < 0) {
    SFU_LOG_WARN("signaling: failed to build server-initiated SDP offer (fd=%d)", fd);
    sfu_remote_offer_manifest_release(manifest);
    return false;
  }
  if (!send_offer_json(fd, s, (size_t)offer_len, manifest->offer_generation)) {
    SFU_LOG_WARN("signaling: failed to send server-initiated offer over WebSocket (fd=%d)", fd);
    sfu_remote_offer_manifest_release(manifest);
    return false;
  }
  if (captured && !sfu_session_remote_offer_install(session, manifest)) {
    SFU_LOG_WARN("signaling: failed to install sent offer manifest (fd=%d generation=%" PRIu64 ")", fd, manifest->offer_generation);
    sfu_remote_offer_manifest_release(manifest);
    return false;
  }
  if (offer_generation) {
    *offer_generation = manifest->offer_generation;
  }
  sfu_remote_offer_manifest_release(manifest);
  return true;
}

void sfu_signaling_notify_media_state(sfu_peer_session_t *peer) {
  if (!peer || !sfu_session_accepts_work(peer)) {
    return;
  }
  sfu_signaling_server_t *server = signaling_producer_acquire();
  if (!server) {
    return;
  }
  if (server->test_membership_only) {
    signaling_producer_release();
    return;
  }
  bool expected = false;
  if (!atomic_compare_exchange_strong_explicit(&peer->media.media_update_queued, &expected, true, memory_order_acq_rel, memory_order_acquire)) {
    signaling_producer_release();
    return;
  }
  atomic_fetch_add_explicit(&peer->refcount, 1, memory_order_relaxed);
  sfu_membership_queue_t *queue = &server->membership_queue;
  pthread_mutex_lock(&queue->lock);
  if (queue->media_count >= SFU_MEMBERSHIP_QUEUE_CAP) {
    pthread_mutex_unlock(&queue->lock);
    atomic_store_explicit(&peer->media.media_update_queued, false, memory_order_release);
    sfu_session_release(peer);
    signaling_producer_release();
    return;
  }
  queue->media_items[queue->media_tail] = peer;
  queue->media_tail = (queue->media_tail + 1) % SFU_MEMBERSHIP_QUEUE_CAP;
  queue->media_count++;
  pthread_mutex_unlock(&queue->lock);
  uv_async_send(&server->renegotiation_waker);
  signaling_producer_release();
}

static bool renegotiation_queue_enqueue_owned(sfu_renegotiation_queue_t *queue, sfu_peer_session_t *session);

static void schedule_peer_renegotiation(sfu_peer_session_t *session, bool bump_revision) {
  if (!session || !sfu_session_accepts_work(session)) {
    return;
  }
  sfu_signaling_server_t *server = signaling_producer_acquire();
  if (!server) {
    return;
  }
  if (server->test_membership_only) {
    signaling_producer_release();
    return;
  }

  atomic_fetch_add_explicit(&session->refcount, 1, memory_order_relaxed);
  uint64_t now_ms = sfu_now_ms();
  pthread_mutex_lock(&session->negotiation.lock);
  if (bump_revision) {
    session->negotiation.desired_offer_revision++;
    if (session->negotiation.desired_offer_revision == 0) {
      session->negotiation.desired_offer_revision = 1;
    }
  }
  session->negotiation.renegotiation_pending = session->negotiation.desired_offer_revision > session->negotiation.answered_revision;
  if (session->state == SFU_SESSION_ESTABLISHED && !session->negotiation.offer_outstanding && session->negotiation.renegotiation_pending) {
    if (!session->negotiation.negotiation_needed) {
      session->negotiation.negotiation_needed = true;
      session->negotiation.negotiation_first_dirty_ms = now_ms;
      session->negotiation.negotiation_due_ms = now_ms + SFU_RENEGOTIATION_DEBOUNCE_MS;
    } else {
      uint64_t due_ms = now_ms + SFU_RENEGOTIATION_DEBOUNCE_MS;
      uint64_t deadline_ms = session->negotiation.negotiation_first_dirty_ms + SFU_RENEGOTIATION_MAX_DELAY_MS;
      uint64_t new_due = due_ms < deadline_ms ? due_ms : deadline_ms;
      session->negotiation.negotiation_due_ms = new_due;
    }
  }
  pthread_mutex_unlock(&session->negotiation.lock);

  if (renegotiation_queue_enqueue_owned(&server->renegotiation_queue, session)) {
    session = NULL;
  }
  if (session) {
    sfu_session_release(session);
  }
  if (!server->suppress_wake) {
    uv_async_send(&server->renegotiation_waker);
  }
  signaling_producer_release();
}

void sfu_signaling_trigger_peer_renegotiation(sfu_peer_session_t *session) { schedule_peer_renegotiation(session, true); }
void sfu_signaling_schedule_pending_peer(sfu_peer_session_t *session) { schedule_peer_renegotiation(session, false); }

bool sfu_signaling_reconcile_remote_slots(sfu_peer_session_t *session) {
  if (!session || !sfu_session_accepts_work(session) || !sfu_session_remote_slots_pending(session, NULL, NULL)) {
    return false;
  }

  pthread_mutex_lock(&session->negotiation.lock);
  if (session->negotiation.desired_offer_revision <= session->negotiation.answered_revision) {
    session->negotiation.desired_offer_revision = session->negotiation.answered_revision + 1;
    if (session->negotiation.desired_offer_revision == 0) {
      session->negotiation.desired_offer_revision = 1;
    }
  }
  session->negotiation.renegotiation_pending = true;
  pthread_mutex_unlock(&session->negotiation.lock);

  schedule_peer_renegotiation(session, false);
  return true;
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
        } else if (strncasecmp(codec, "H264", 4) == 0) {
          section.codecs[pt] = SFU_VIDEO_CODEC_H264;
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

static void disconnect_client(sfu_client_conn_t *c, sfu_disconnect_reason_t reason);

static void register_client(sfu_client_conn_t *c) {
  if (!c || !c->server || c->in_registry) {
    return;
  }
  c->registry_next = c->server->connections_head;
  if (c->registry_next) {
    c->registry_next->registry_prev = c;
  }
  c->server->connections_head = c;
  c->in_registry = true;
}

static void unregister_client(sfu_client_conn_t *c) {
  if (!c || !c->server || !c->in_registry) {
    return;
  }
  if (c->registry_prev) {
    c->registry_prev->registry_next = c->registry_next;
  } else {
    c->server->connections_head = c->registry_next;
  }
  if (c->registry_next) {
    c->registry_next->registry_prev = c->registry_prev;
  }
  c->registry_prev = NULL;
  c->registry_next = NULL;
  c->in_registry = false;
}

static void finish_client_close(sfu_client_conn_t *c) {
  SFU_LOG_INFO("signaling: client closed fd=%d ufrag=%s", c->fd, c->client_ufrag);
  unregister_client(c);

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
    (void)sfu_session_begin_close(c->server->sessions, session);
    sfu_session_release(session);
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
    disconnect_client(c, SFU_DISCONNECT_IDLE_TIMEOUT);
    return;
  }

  static const char ping_msg[] = "{\"type\":\"ping\"}";
  if (sfu_ws_send_text(c->fd, ping_msg, sizeof(ping_msg) - 1) != 0) {
    SFU_LOG_WARN("signaling: failed to send ping fd=%d; closing", c->fd);
    disconnect_client(c, SFU_DISCONNECT_PING_FAILED);
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

static void disconnect_client(sfu_client_conn_t *c, sfu_disconnect_reason_t reason) {
  if (!c || c->disconnecting) {
    return;
  }
  c->disconnecting = true;

  if (c->handshake_done) {
    sfu_ws_send_close(c->fd, (uint16_t)reason, NULL, 0);
  }

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
    disconnect_client(c, SFU_DISCONNECT_PING_FAILED);
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
    disconnect_client(c, SFU_DISCONNECT_AUTH_NOT_CONFIGURED);
    return;
  }

  int token_len = sfu_json_extract_string(buf, n, "token", token, sizeof(token));
  if (token_len < 0) {
    SFU_LOG_WARN("signaling: join missing token (fd=%d)", c->fd);
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"missing_token\"}", 42);
    disconnect_client(c, SFU_DISCONNECT_MISSING_TOKEN);
    return;
  }
  uint64_t token_room = 0;
  if (sfu_handshake_verify_join_token(token, (size_t)token_len, jwt_secret, &user_id, &token_room) != 0) {
    SFU_LOG_WARN("signaling: join JWT invalid (fd=%d)", c->fd);
    sfu_ws_send_text(c->fd, "{\"type\":\"error\",\"message\":\"invalid_token\"}", 42);
    disconnect_client(c, SFU_DISCONNECT_INVALID_TOKEN);
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
  SFU_LOG_INFO("signaling: join role user_id=%" PRId64 " room=%" PRIu64 " role=%s audience=%d fd=%d", c->user_id, room_id,
               role_str[0] != '\0' ? role_str : "<missing>", c->is_audience, c->fd);

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

  uint64_t received_offer_generation = 0;
  if (sfu_json_extract_uint64(buf, n, "offer_generation", &received_offer_generation) != 0) {
    static const char missing_generation[] = "{\"type\":\"error\",\"message\":\"missing_offer_generation\"}";
    sfu_ws_send_text(c->fd, missing_generation, sizeof(missing_generation) - 1);
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
  if (!have_ufrag && c->client_ufrag[0] == '\0') {
    SFU_LOG_WARN("signaling: initial answer has no ICE ufrag (fd=%d), rejecting", c->fd);
    static const char invalid_answer[] = "{\"type\":\"error\",\"message\":\"invalid_answer_ufrag\"}";
    sfu_ws_send_text(c->fd, invalid_answer, sizeof(invalid_answer) - 1);
    return;
  }
  const char *lookup_ufrag = c->client_ufrag[0] != '\0' ? c->client_ufrag : answer_ufrag;

  sfu_answer_media_t media;
  if (!parse_answer_media(sdp, (size_t)sdp_len, &media)) {
    static const char invalid_answer[] = "{\"type\":\"error\",\"message\":\"invalid_answer_sdp\"}";
    sfu_ws_send_text(c->fd, invalid_answer, sizeof(invalid_answer) - 1);
    return;
  }

  sfu_remote_offer_manifest_t *answered_manifest = NULL;
  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(s->sessions, lookup_ufrag);
  if (session) {
    pthread_mutex_lock(&session->graph.lock);
    sfu_remote_offer_manifest_t *current = session->graph.remote_slots.offered_manifest;
    uint64_t expected_generation = current ? current->offer_generation : 0;
    if (current && received_offer_generation == expected_generation) {
      answered_manifest = current;
      sfu_remote_offer_manifest_retain(answered_manifest);
    }
    pthread_mutex_unlock(&session->graph.lock);
    pthread_mutex_lock(&session->negotiation.lock);
    uint64_t last_answered_generation = session->negotiation.last_answered_offer_generation;
    bool generation_valid = received_offer_generation != 0 && answered_manifest != NULL && session->negotiation.offer_outstanding &&
                            received_offer_generation > last_answered_generation;
    pthread_mutex_unlock(&session->negotiation.lock);
    if (!generation_valid) {
      const char *reason = received_offer_generation == 0 || received_offer_generation <= last_answered_generation ||
                                   (expected_generation != 0 && received_offer_generation < expected_generation)
                               ? "stale_offer_generation"
                               : "future_offer_generation";
      char response[112];
      int response_len = snprintf(response, sizeof(response), "{\"type\":\"error\",\"message\":\"%s\"}", reason);
      if (response_len > 0 && (size_t)response_len < sizeof(response)) {
        sfu_ws_send_text(c->fd, response, (size_t)response_len);
      }
      sfu_remote_offer_manifest_release(answered_manifest);
      sfu_session_release(session);
      return;
    }
  } else if (received_offer_generation != 0 || c->initial_answer_accepted) {
    /* Captured (nonzero) manifests are only sent after a session exists. Generation 0 is the pre-STUN initial offer. */
    const char *message = c->initial_answer_accepted ? "stale_offer_generation" : "future_offer_generation";
    char response[112];
    int response_len = snprintf(response, sizeof(response), "{\"type\":\"error\",\"message\":\"%s\"}", message);
    if (response_len > 0 && (size_t)response_len < sizeof(response)) {
      sfu_ws_send_text(c->fd, response, (size_t)response_len);
    }
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

  sfu_routing_answer_reservation_t routing_reservation;
  sfu_routing_register_result_t register_result =
      sfu_routing_table_prepare_answer(s->routing_table, lookup_ufrag, c->joined_room, c->fd, &pending, &routing_reservation);
  if (register_result != SFU_ROUTING_REGISTER_OK) {
    const char *message = register_result == SFU_ROUTING_REGISTER_OWNERSHIP_CONFLICT ? "duplicate_ice_ufrag"
                          : register_result == SFU_ROUTING_REGISTER_TABLE_FULL       ? "routing_table_full"
                                                                                     : "routing_registration_failed";
    char response[96];
    int response_len = snprintf(response, sizeof(response), "{\"type\":\"error\",\"message\":\"%s\"}", message);
    if (response_len > 0 && (size_t)response_len < sizeof(response)) {
      sfu_ws_send_text(c->fd, response, (size_t)response_len);
    }
    sfu_remote_offer_manifest_release(answered_manifest);
    if (session) {
      sfu_session_release(session);
    }
    return;
  }

  if (c->client_ufrag[0] == '\0') {
    snprintf(c->client_ufrag, sizeof(c->client_ufrag), "%s", answer_ufrag);
  }
  uint32_t answer_generation = 0;
  if (!sfu_routing_table_commit_answer(&routing_reservation, &answer_generation)) {
    static const char routing_failed[] = "{\"type\":\"error\",\"message\":\"routing_registration_failed\"}";
    sfu_ws_send_text(c->fd, routing_failed, sizeof(routing_failed) - 1);
    sfu_remote_offer_manifest_release(answered_manifest);
    if (session) {
      sfu_session_release(session);
    }
    return;
  }
  SFU_LOG_INFO("signaling: atomically registered answer ufrag=%s -> room_id=%" PRIu64 " generation=%u", c->client_ufrag, c->joined_room_id, answer_generation);

  if (answered_manifest) {
    bool applied = sfu_session_remote_offer_apply_answer(session, answered_manifest);
    sfu_remote_offer_manifest_release(answered_manifest);
    answered_manifest = NULL;
    if (!applied) {
      static const char stale_generation[] = "{\"type\":\"error\",\"message\":\"stale_offer_generation\"}";
      sfu_ws_send_text(c->fd, stale_generation, sizeof(stale_generation) - 1);
      sfu_session_release(session);
      return;
    }
  }

  if (!session) {
    c->initial_answer_accepted = true;
    SFU_LOG_INFO("signaling: answer for ufrag=%s awaits authenticated STUN bind", c->client_ufrag);
    return;
  }

  SFU_LOG_INFO("signaling: answer role ufrag=%s peer_id=%u pending_audience=%d session_audience=%d", c->client_ufrag, session->peer_id, pending.is_audience,
               atomic_load_explicit(&session->is_audience, memory_order_acquire));

  bool follow_up_pending = false;
  uint64_t answered_offer_generation = 0;
  uint64_t answered_offer_revision = 0;
  pthread_mutex_lock(&session->negotiation.lock);
  if (session->negotiation.offer_outstanding) {
    session->negotiation.offer_outstanding = false;
    answered_offer_generation = session->negotiation.offer_generation;
    answered_offer_revision = session->negotiation.offered_revision;
    session->negotiation.answered_revision = answered_offer_revision;
    session->negotiation.last_answered_offer_generation = received_offer_generation;
  }
  follow_up_pending = session->negotiation.desired_offer_revision > session->negotiation.answered_revision;
  session->negotiation.renegotiation_pending = follow_up_pending;
  pthread_mutex_unlock(&session->negotiation.lock);
  if (answered_offer_generation != 0) {
    SFU_LOG_INFO("signaling: completed renegotiation answer ufrag=%s peer_id=%u generation=%" PRIu64 " revision=%" PRIu64 " pending=%d", c->client_ufrag,
                 session->peer_id, answered_offer_generation, answered_offer_revision, follow_up_pending);
  }

  bool role_changed = false;
  bool media_changed = false;
  bool newly_bound = false;
  bool sdp_contract_changed = false;
  sfu_media_snapshot_t media_before_answer = sfu_session_load_media(session);
  pthread_mutex_lock(&session->media.lock);
  sdp_contract_changed = (pending.video_pt != 0 && pending.video_pt != session->media.uplink_video.payload_type) ||
                         (pending.rtx_pt != 0 && pending.rtx_pt != session->media.uplink_video.rtx_payload_type) ||
                         (pending.video_codec != SFU_VIDEO_CODEC_NONE && pending.video_codec != (uint8_t)session->media.uplink_video.codec) ||
                         (pending.screen_pt != 0 && pending.screen_pt != session->media.screen.payload_type) ||
                         (pending.screen_rtx_pt != 0 && pending.screen_rtx_pt != session->media.screen.rtx_payload_type) ||
                         (pending.screen_codec != SFU_VIDEO_CODEC_NONE && pending.screen_codec != (uint8_t)session->media.screen.codec);
  pthread_mutex_unlock(&session->media.lock);
  if (sfu_routing_table_reconcile_answer(s->routing_table, c->client_ufrag, c->joined_room, c->fd, answer_generation, session, &role_changed, &media_changed)) {
    if (!session->room) {
      sfu_room_admission_result_t admission = room_add_peer_result(c->joined_room, session);
      newly_bound = admission == SFU_ROOM_ADMISSION_OK;
      if (admission == SFU_ROOM_ADMISSION_CAPACITY) {
        static const char capacity[] = "{\"type\":\"error\",\"message\":\"room_capacity\"}";
        (void)sfu_ws_send_text(c->fd, capacity, sizeof(capacity) - 1);
        SFU_LOG_WARN("answer: room admission capacity exhausted ufrag=%s room=%" PRIu64, c->client_ufrag, c->joined_room_id);
      }
    }
    if (session->room && (media_changed || role_changed || newly_bound)) {
      SFU_LOG_INFO("answer: media/role changed for ufrag=%s generation=%u (media=%d role=%d bound=%d), refreshing forwarding", c->client_ufrag,
                   answer_generation, media_changed, role_changed, newly_bound);
      room_refresh_peer_streams((sfu_room_t *)session->room, session);
    }
    sfu_media_snapshot_t media_after_answer = sfu_session_load_media(session);
    bool camera_activity_changed = media_before_answer.video_active != media_after_answer.video_active;
    bool screen_activity_changed = media_before_answer.screen_active != media_after_answer.screen_active;
    if (session->room && (camera_activity_changed || screen_activity_changed)) {
      broadcast_peer_updated((sfu_room_t *)session->room, session);
      if (camera_activity_changed && !newly_bound) {
        atomic_store_explicit(&session->media.camera_announced_active, media_after_answer.video_active, memory_order_release);
        emit_hook_event(media_after_answer.video_active ? "publish" : "unpublish", session->user_id, c->joined_room_id);
      }
      if (screen_activity_changed && !newly_bound) {
        atomic_store_explicit(&session->media.screen_announced_active, media_after_answer.screen_active, memory_order_release);
        emit_hook_event(media_after_answer.screen_active ? "share_screen" : "unshare_screen", session->user_id, c->joined_room_id);
      }
    }
  }
  uint32_t active_unapplied = 0;
  uint32_t obsolete_applied = 0;
  bool downlink_pending = sfu_session_remote_slots_pending(session, &active_unapplied, &obsolete_applied);
  bool convergence_scheduled = sfu_signaling_reconcile_remote_slots(session);
  if (answered_offer_generation != 0) {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(session);
    uint32_t receiver_count = snap ? snap->count : 0;
    pthread_mutex_lock(&session->media.lock);
    uint8_t mid_recv_extmap_id = session->media.mid_recv_extmap_id;
    pthread_mutex_unlock(&session->media.lock);
#ifdef SFU_DIAG_LOG
    uint32_t high_water = sfu_session_remote_slot_high_water(session);
    uint64_t applied0 = high_water > 0 ? atomic_load_explicit(&session->graph.remote_slots.applied_assignment_generations[0], memory_order_acquire) : 0;
    uint64_t applied1 = high_water > 1 ? atomic_load_explicit(&session->graph.remote_slots.applied_assignment_generations[1], memory_order_acquire) : 0;
    SFU_LOG_INFO(
        "signaling: downlink %s ufrag=%s peer_id=%u audience=%d receivers=%u high_water=%u mid_extmap=%u"
        " active_unapplied=%u obsolete_applied=%u applied[0]=%" PRIu64 " applied[1]=%" PRIu64 " offer_gen=%" PRIu64,
        downlink_pending ? "pending follow-up" : "converged", c->client_ufrag, session->peer_id,
        atomic_load_explicit(&session->is_audience, memory_order_acquire), receiver_count, high_water, mid_recv_extmap_id, active_unapplied, obsolete_applied,
        applied0, applied1, answered_offer_generation);
#else
    SFU_LOG_INFO("signaling: downlink %s ufrag=%s peer_id=%u audience=%d receivers=%u mid_extmap=%u active_unapplied=%u obsolete_applied=%u",
                 downlink_pending ? "pending follow-up" : "converged", c->client_ufrag, session->peer_id,
                 atomic_load_explicit(&session->is_audience, memory_order_acquire), receiver_count, mid_recv_extmap_id, active_unapplied, obsolete_applied);
#endif
    if (snap) {
      sfu_subscriptions_snapshot_release(snap);
    }
  }
  if (sdp_contract_changed && session->room) {
  }
  if (follow_up_pending && !convergence_scheduled) {
    schedule_peer_renegotiation(session, false);
  }
  sfu_session_release(session);
}

static void send_peer_updated(int fd, sfu_peer_session_t *session) {
  if (fd < 0 || !session) {
    return;
  }
  bool is_audience = atomic_load_explicit(&session->is_audience, memory_order_acquire);
  bool is_mute = atomic_load_explicit(&session->media.is_mute, memory_order_acquire);
  bool camera_requested = atomic_load_explicit(&session->media.camera_enabled, memory_order_acquire);
  bool screen_requested = atomic_load_explicit(&session->media.screen_enabled, memory_order_acquire);
  sfu_media_snapshot_t media = sfu_session_load_media(session);
  char event[448];
  int n = snprintf(event, sizeof(event),
                   "{\"type\":\"peer_updated\",\"peer\":{\"peer_id\":%u,\"user_id\":\"%" PRId64
                   "\",\"role\":\"%s\",\"is_mute\":%s,\"camera_requested\":%s,\"camera_active\":%s,"
                   "\"screen_requested\":%s,\"screen_active\":%s}}",
                   session->peer_id, session->user_id, is_audience ? "audience" : "speaker", is_mute ? "true" : "false", camera_requested ? "true" : "false",
                   media.video_active ? "true" : "false", screen_requested ? "true" : "false", media.screen_active ? "true" : "false");
  if (n > 0 && (size_t)n < sizeof(event)) {
    (void)sfu_ws_send_text(fd, event, (size_t)n);
  }
}

static void broadcast_peer_updated(sfu_room_t *room, sfu_peer_session_t *session) {
  if (!room || !session) {
    return;
  }
  int fds[SFU_ROOM_MAX_PEERS];
  uint32_t count = 0;
  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->peer_capacity && count < SFU_ROOM_MAX_PEERS; i++) {
    sfu_peer_session_t *peer = room->peers[i];
    if (peer && peer->fd >= 0 && sfu_session_accepts_work(peer)) {
      fds[count++] = peer->fd;
    }
  }
  pthread_mutex_unlock(&room->lock);

  for (uint32_t i = 0; i < count; i++) {
    send_peer_updated(fds[i], session);
  }
}

static void flush_media_state_events(sfu_signaling_server_t *s) {
  sfu_peer_session_t *session;
  while ((session = media_update_queue_pop(&s->membership_queue)) != NULL) {
    atomic_store_explicit(&session->media.media_update_queued, false, memory_order_release);
    sfu_media_snapshot_t media = sfu_session_load_media(session);
    bool old_camera = atomic_exchange_explicit(&session->media.camera_announced_active, media.video_active, memory_order_acq_rel);
    bool old_screen = atomic_exchange_explicit(&session->media.screen_announced_active, media.screen_active, memory_order_acq_rel);
    bool camera_changed = old_camera != media.video_active;
    bool screen_changed = old_screen != media.screen_active;
    if (session->room && (camera_changed || screen_changed)) {
      broadcast_peer_updated((sfu_room_t *)session->room, session);
      uint64_t room_id = ((sfu_room_t *)session->room)->room_id;
      if (camera_changed) {
        emit_hook_event(media.video_active ? "publish" : "unpublish", session->user_id, room_id);
      }
      if (screen_changed) {
        emit_hook_event(media.screen_active ? "share_screen" : "unshare_screen", session->user_id, room_id);
      }
    }
    sfu_session_release(session);
  }
}

static void handle_push_to_talk(sfu_client_conn_t *c, const char *buf, size_t n) {
  bool active = false;
  if (!c->joined_room || c->client_ufrag[0] == '\0' || sfu_json_extract_bool(buf, n, "active", &active) != 0) {
    static const char invalid_ptt[] = "{\"type\":\"error\",\"message\":\"invalid_push_to_talk\"}";
    sfu_ws_send_text(c->fd, invalid_ptt, sizeof(invalid_ptt) - 1);
    return;
  }

  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(c->server->sessions, c->client_ufrag);
  bool has_session = session != NULL;
  bool in_room = has_session && session->room == c->joined_room;
  bool is_audience = has_session && atomic_load_explicit(&session->is_audience, memory_order_acquire);
  bool audio_send_negotiated = has_session && atomic_load_explicit(&session->media.audio_send_negotiated, memory_order_acquire);
  bool negotiation_ok = !active || audio_send_negotiated;
  bool accepted = has_session && in_room && is_audience && negotiation_ok && room_set_peer_ptt_active(c->joined_room, session, active);
  if (!accepted) {
    static const char rejected[] = "{\"type\":\"error\",\"message\":\"push_to_talk_rejected\"}";
    sfu_ws_send_text(c->fd, rejected, sizeof(rejected) - 1);

#ifdef SFU_DIAG_LOG
    if (!has_session) {
      SFU_LOG_WARN("signaling: push_to_talk rejected user_id=%" PRId64 " ufrag=%s requested_active=%d reason=no_session", c->user_id, c->client_ufrag, active);
    } else {
      sfu_media_snapshot_t snap = sfu_session_load_media(session);
      bool ptt_active = atomic_load_explicit(&session->media.ptt_active, memory_order_acquire);
      uint32_t gen = session->cold ? session->cold->transport_generation : 0;
      SFU_LOG_WARN("signaling: push_to_talk rejected user_id=%" PRId64
                   " ufrag=%s peer_id=%u requested_active=%d has_session=%d in_room=%d "
                   "is_audience=%d audio_send_negotiated=%d negotiation_ok=%d ptt_active=%d audio_ssrc=%u audio_active=%d generation=%u",
                   c->user_id, c->client_ufrag, session->peer_id, active, has_session, in_room, is_audience, audio_send_negotiated, negotiation_ok, ptt_active,
                   snap.audio_ssrc, snap.audio_active, gen);
    }
#endif

  } else {
    char response[80];
    int response_len = snprintf(response, sizeof(response), "{\"type\":\"push_to_talk_changed\",\"active\":%s}", active ? "true" : "false");
    if (response_len > 0 && (size_t)response_len < sizeof(response)) {
      sfu_ws_send_text(c->fd, response, (size_t)response_len);
    }

#ifdef SFU_DIAG_LOG
    sfu_media_snapshot_t snap = sfu_session_load_media(session);
    bool ptt_active = atomic_load_explicit(&session->media.ptt_active, memory_order_acquire);
    bool vis = atomic_load_explicit(&session->media.visible, memory_order_acquire);
    uint32_t gen = session->cold ? session->cold->transport_generation : 0;
    SFU_LOG_INFO("signaling: push_to_talk user_id=%" PRId64
                 " ufrag=%s peer_id=%u active=%d role=audience ptt_active=%d visible=%d "
                 "audio_ssrc=%u audio_active=%d generation=%u audio_send_negotiated=%d",
                 c->user_id, c->client_ufrag, session->peer_id, active, ptt_active, vis, snap.audio_ssrc, snap.audio_active, gen, audio_send_negotiated);
#endif
  }
  if (session) {
    sfu_session_release(session);
  }
}

static void handle_role_change(sfu_client_conn_t *c, sfu_signaling_server_t *s, const char *buf, size_t n) {
  (void)s;
  (void)buf;
  (void)n;
  if (!c->joined_room || c->client_ufrag[0] == '\0') {
    return;
  }
  static const char role_change_disabled[] = "{\"type\":\"error\",\"message\":\"role_change_disabled\"}";
  sfu_ws_send_text(c->fd, role_change_disabled, sizeof(role_change_disabled) - 1);
  SFU_LOG_INFO("signaling: role_change rejected (not supported) user_id=%" PRId64 " (fd=%d)", c->user_id, c->fd);
}

static void handle_camera(sfu_client_conn_t *c, const char *buf, size_t n) {
  bool requested = false;
  if (!c->joined_room || c->client_ufrag[0] == '\0' || sfu_json_extract_bool(buf, n, "active", &requested) != 0) {
    static const char invalid[] = "{\"type\":\"error\",\"message\":\"invalid_camera_active\"}";
    sfu_ws_send_text(c->fd, invalid, sizeof(invalid) - 1);
    return;
  }
  if (c->is_audience) {
    static const char unavailable[] = "{\"type\":\"error\",\"message\":\"camera_not_allowed\"}";
    sfu_ws_send_text(c->fd, unavailable, sizeof(unavailable) - 1);
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

  bool previous_requested = atomic_exchange_explicit(&session->media.camera_enabled, requested, memory_order_acq_rel);
  bool requested_changed = previous_requested != requested;
  pthread_mutex_lock(&session->media.lock);
  bool previous_active = session->media.uplink_video.active;
  if (!requested) {
    atomic_store_explicit(&session->media.camera_rtp_observed, false, memory_order_release);
  }
  bool effective_changed = sfu_session_recompute_video_activity_locked(session);
  bool effective_active = session->media.uplink_video.active;
  if (effective_changed) {
    sfu_session_publish_media(session);
  }
  pthread_mutex_unlock(&session->media.lock);

  if (effective_changed) {
    atomic_store_explicit(&session->media.camera_announced_active, effective_active, memory_order_release);
    room_refresh_peer_streams(c->joined_room, session);
    emit_hook_event(effective_active ? "publish" : "unpublish", c->user_id, c->joined_room_id);
  }
  if (requested_changed || effective_changed) {
    broadcast_peer_updated(c->joined_room, session);
  } else {
    send_peer_updated(c->fd, session);
  }

  SFU_LOG_INFO("signaling: camera user_id=%" PRId64 " requested=%d active=%d previous_active=%d", c->user_id, requested, effective_active, previous_active);
  sfu_session_release(session);
}

static void handle_screen_share(sfu_client_conn_t *c, const char *buf, size_t n) {
  bool requested = false;
  if (!c->joined_room || c->client_ufrag[0] == '\0' || sfu_json_extract_bool(buf, n, "active", &requested) != 0) {
    static const char invalid[] = "{\"type\":\"error\",\"message\":\"invalid_screen_share_active\"}";
    sfu_ws_send_text(c->fd, invalid, sizeof(invalid) - 1);
    return;
  }
  if (c->is_audience) {
    static const char unavailable[] = "{\"type\":\"error\",\"message\":\"screen_share_not_allowed\"}";
    sfu_ws_send_text(c->fd, unavailable, sizeof(unavailable) - 1);
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

  bool screen_negotiated = atomic_load_explicit(&session->media.screen_send_negotiated, memory_order_acquire);
  bool previous_requested = atomic_exchange_explicit(&session->media.screen_enabled, requested, memory_order_acq_rel);
  bool requested_changed = previous_requested != requested;
  pthread_mutex_lock(&session->media.lock);
  if (!requested) {
    atomic_store_explicit(&session->media.screen_rtp_observed, false, memory_order_release);
  }
  bool effective_changed = sfu_session_recompute_video_activity_locked(session);
  bool effective_active = session->media.screen.active;
  if (effective_changed) {
    sfu_session_publish_media(session);
  }
  pthread_mutex_unlock(&session->media.lock);

  if (effective_changed) {
    atomic_store_explicit(&session->media.screen_announced_active, effective_active, memory_order_release);
    room_refresh_peer_streams(c->joined_room, session);
    emit_hook_event(effective_active ? "share_screen" : "unshare_screen", c->user_id, c->joined_room_id);
  }
  if (requested_changed || effective_changed) {
    broadcast_peer_updated(c->joined_room, session);
  } else {
    send_peer_updated(c->fd, session);
  }
  if (requested && requested_changed && !screen_negotiated) {
    SFU_LOG_INFO("signaling: screen share requires sender negotiation ufrag=%s peer_id=%u", c->client_ufrag, session->peer_id);
    sfu_signaling_trigger_peer_renegotiation(session);
  }

  sfu_session_release(session);
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

  atomic_store_explicit(&session->media.visible, visible, memory_order_release);
  sfu_session_release(session);

  char response[96];
  int response_len = snprintf(response, sizeof(response), "{\"type\":\"visibility_changed\",\"visible\":%s}", visible ? "true" : "false");
  if (response_len > 0 && (size_t)response_len < sizeof(response)) {
    sfu_ws_send_text(c->fd, response, (size_t)response_len);
  }
  SFU_LOG_INFO("signaling: visibility user_id=%" PRId64 " visible=%s (fd=%d)", c->user_id, visible ? "true" : "false", c->fd);
}

static void send_participant_action_completed(sfu_client_conn_t *c, const char *action, int64_t user_id, uint32_t affected) {
  char response[160];
  int response_len =
      snprintf(response, sizeof(response), "{\"type\":\"participant_action_completed\",\"action\":\"%s\",\"user_id\":\"%" PRId64 "\",\"affected\":%u}", action,
               user_id, affected);
  if (response_len > 0 && (size_t)response_len < sizeof(response)) {
    (void)sfu_ws_send_text(c->fd, response, (size_t)response_len);
  }
}

static void handle_participant_action(sfu_client_conn_t *c, const char *buf, size_t n) {
  if (!c->joined_room || c->joined_room_id == 0 || c->joined_room->room_id != c->joined_room_id) {
    static const char must_join[] = "{\"type\":\"error\",\"message\":\"must_join_room_first\"}";
    sfu_ws_send_text(c->fd, must_join, sizeof(must_join) - 1);
    return;
  }

  char token[4096];
  int token_len = sfu_json_extract_string(buf, n, "token", token, sizeof(token));
  if (token_len < 0) {
    static const char invalid[] = "{\"type\":\"error\",\"message\":\"invalid_participant_action\"}";
    sfu_ws_send_text(c->fd, invalid, sizeof(invalid) - 1);
    return;
  }

  const char *jwt_secret = g_sfu_config.jwt_secret;
  if (jwt_secret[0] == '\0') {
    static const char auth_missing[] = "{\"type\":\"error\",\"message\":\"auth_not_configured\"}";
    sfu_ws_send_text(c->fd, auth_missing, sizeof(auth_missing) - 1);
    return;
  }

  sfu_jwt_claims_t claims;
  if (sfu_handshake_verify_token_claims(token, (size_t)token_len, jwt_secret, &claims) != 0) {
    static const char invalid_token[] = "{\"type\":\"error\",\"message\":\"invalid_token\"}";
    sfu_ws_send_text(c->fd, invalid_token, sizeof(invalid_token) - 1);
    return;
  }
  if (claims.room_id != c->joined_room_id) {
    static const char room_mismatch[] = "{\"type\":\"error\",\"message\":\"token_room_mismatch\"}";
    sfu_ws_send_text(c->fd, room_mismatch, sizeof(room_mismatch) - 1);
    return;
  }

  if (strcmp(claims.metadata, "mute") == 0) {
    sfu_peer_session_t *targets[SFU_ROOM_MAX_PEERS];
    uint32_t target_count = 0;
    pthread_mutex_lock(&c->joined_room->lock);
    for (uint32_t i = 0; i < c->joined_room->peer_capacity && target_count < SFU_ROOM_MAX_PEERS; i++) {
      sfu_peer_session_t *peer = c->joined_room->peers[i];
      if (peer && peer->room == c->joined_room && peer->user_id == claims.user_id && sfu_session_accepts_work(peer)) {
        atomic_fetch_add_explicit(&peer->refcount, 1, memory_order_relaxed);
        targets[target_count++] = peer;
      }
    }
    pthread_mutex_unlock(&c->joined_room->lock);

    if (target_count == 0) {
      static const char target_not_found[] = "{\"type\":\"error\",\"message\":\"target_not_found\"}";
      sfu_ws_send_text(c->fd, target_not_found, sizeof(target_not_found) - 1);
      return;
    }

    for (uint32_t i = 0; i < target_count; i++) {
      atomic_store_explicit(&targets[i]->media.is_mute, true, memory_order_release);
      broadcast_peer_updated(c->joined_room, targets[i]);
      sfu_session_release(targets[i]);
    }
    send_participant_action_completed(c, "mute", claims.user_id, target_count);
    SFU_LOG_INFO("signaling: participant action=mute target_user_id=%" PRId64 " room=%" PRIu64 " affected=%u (fd=%d)", claims.user_id, claims.room_id,
                 target_count, c->fd);
    return;
  }

  if (strcmp(claims.metadata, "kick") == 0) {
    uint32_t target_count = 0;
    for (sfu_client_conn_t *target = c->server->connections_head; target; target = target->registry_next) {
      if (!target->disconnecting && target->joined_room == c->joined_room && target->joined_room_id == claims.room_id && target->user_id == claims.user_id) {
        target_count++;
      }
    }
    if (target_count == 0) {
      static const char target_not_found[] = "{\"type\":\"error\",\"message\":\"target_not_found\"}";
      sfu_ws_send_text(c->fd, target_not_found, sizeof(target_not_found) - 1);
      return;
    }

    send_participant_action_completed(c, "kick", claims.user_id, target_count);
    SFU_LOG_INFO("signaling: participant action=kick target_user_id=%" PRId64 " room=%" PRIu64 " affected=%u (fd=%d)", claims.user_id, claims.room_id,
                 target_count, c->fd);
    for (sfu_client_conn_t *target = c->server->connections_head; target;) {
      sfu_client_conn_t *next = target->registry_next;
      if (!target->disconnecting && target->joined_room == c->joined_room && target->joined_room_id == claims.room_id && target->user_id == claims.user_id) {
        disconnect_client(target, SFU_DISCONNECT_KICKED);
      }
      target = next;
    }
    return;
  }

  static const char unsupported[] = "{\"type\":\"error\",\"message\":\"unsupported_participant_action\"}";
  sfu_ws_send_text(c->fd, unsupported, sizeof(unsupported) - 1);
}

static void handle_self_mute(sfu_client_conn_t *c, const char *buf, size_t n) {
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

  atomic_store_explicit(&session->media.is_mute, is_mute, memory_order_release);
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
  } else if (strcmp(type, "push_to_talk") == 0) {
    handle_push_to_talk(c, buf, n);
  } else if (strcmp(type, "role_change") == 0) {
    handle_role_change(c, s, buf, n);
  } else if (strcmp(type, "visibility") == 0) {
    handle_visibility(c, buf, n);
  } else if (strcmp(type, "mute") == 0) {
    handle_self_mute(c, buf, n);
  } else if (strcmp(type, "participant_action") == 0) {
    handle_participant_action(c, buf, n);
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
    disconnect_client(c, SFU_DISCONNECT_TRANSPORT_ERROR);
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

    if (sfu_ws_handshake(c->fd, &c->ws_read) != 0) {
      SFU_LOG_WARN("signaling: WebSocket handshake failed");
      disconnect_client(c, SFU_DISCONNECT_WS_HANDSHAKE_FAILED);
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
    if (!sfu_ws_read_state_has_pending(&c->ws_read)) {
      return;
    }
  }

  do {
    char *buf = s->scratch.recv;
    ssize_t n = sfu_ws_recv_text(c->fd, &c->ws_read, buf, SFU_SIGNALING_RECV_CAP);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      disconnect_client(c, SFU_DISCONNECT_RECV_ERROR);
      return;
    }
    if (n == 0) {
      disconnect_client(c, SFU_DISCONNECT_RECV_ERROR);
      return;
    }

    mark_client_activity(c);
    dispatch_client_message(c, s, buf, (size_t)n);
    if (c->disconnecting) {
      return;
    }
  } while (sfu_ws_read_state_has_pending(&c->ws_read));
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
      register_client(c);

      rc = uv_poll_start(&c->poll_handle, UV_READABLE, on_client_readable);
      if (rc != 0) {
        SFU_LOG_ERROR("signaling: uv_poll_start failed: %s", uv_strerror(rc));
        disconnect_client(c, SFU_DISCONNECT_POLL_START_FAILED);
      }
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      SFU_LOG_ERROR("signaling: accept failed: %s", strerror(errno));
    }
  }
}

static sfu_peer_session_t *media_update_queue_pop(sfu_membership_queue_t *queue) {
  pthread_mutex_lock(&queue->lock);
  sfu_peer_session_t *session = NULL;
  if (queue->media_count > 0) {
    session = queue->media_items[queue->media_head];
    queue->media_items[queue->media_head] = NULL;
    queue->media_head = (queue->media_head + 1) % SFU_MEMBERSHIP_QUEUE_CAP;
    queue->media_count--;
  }
  pthread_mutex_unlock(&queue->lock);
  return session;
}

static sfu_membership_event_t *membership_queue_pop(sfu_membership_queue_t *queue) {
  pthread_mutex_lock(&queue->lock);
  sfu_membership_event_t *event = NULL;
  if (queue->count > 0) {
    event = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % SFU_MEMBERSHIP_QUEUE_CAP;
    queue->count--;
    pthread_cond_signal(&queue->not_full);
  }
  pthread_mutex_unlock(&queue->lock);
  return event;
}

void sfu_signaling_membership_test_server_init(sfu_signaling_server_t *s) {
  memset(s, 0, sizeof(*s));
  assert(pthread_mutex_init(&s->membership_queue.lock, NULL) == 0);
  assert(pthread_cond_init(&s->membership_queue.not_full, NULL) == 0);
  s->membership_queue.accepting = true;
  s->suppress_wake = true;
  s->test_membership_only = true;
  pthread_mutex_lock(&g_signaling_producer_lock);
  assert(g_signaling_server == NULL && g_signaling_producers == 0);
  g_signaling_stopping = false;
  g_signaling_server = s;
  pthread_mutex_unlock(&g_signaling_producer_lock);
}

sfu_membership_event_t *sfu_signaling_membership_test_pop(sfu_signaling_server_t *s) { return membership_queue_pop(&s->membership_queue); }

void sfu_signaling_membership_test_server_stop(sfu_signaling_server_t *s) {
  pthread_mutex_lock(&g_signaling_producer_lock);
  g_signaling_stopping = true;
  if (g_signaling_server == s) {
    g_signaling_server = NULL;
  }
  pthread_mutex_unlock(&g_signaling_producer_lock);
  pthread_mutex_lock(&s->membership_queue.lock);
  s->membership_queue.accepting = false;
  pthread_cond_broadcast(&s->membership_queue.not_full);
  pthread_mutex_unlock(&s->membership_queue.lock);
  pthread_mutex_lock(&g_signaling_producer_lock);
  while (g_signaling_producers != 0) {
    pthread_cond_wait(&g_signaling_producer_idle, &g_signaling_producer_lock);
  }
  pthread_mutex_unlock(&g_signaling_producer_lock);
  sfu_membership_event_t *event;
  while ((event = membership_queue_pop(&s->membership_queue)) != NULL) {
    sfu_membership_event_release(event);
  }
  pthread_cond_destroy(&s->membership_queue.not_full);
  pthread_mutex_destroy(&s->membership_queue.lock);
}

static bool membership_recipient_valid(const sfu_membership_recipient_t *recipient) {
  if (!recipient || !recipient->session || recipient->fd < 0 || recipient->session->fd != recipient->fd || !sfu_session_accepts_work(recipient->session)) {
    return false;
  }
  if (recipient->assignment_generation == 0) {
    return true;
  }
  if (recipient->remote_slot >= SFU_MAX_REMOTE_SLOTS) {
    return false;
  }
  pthread_mutex_lock(&recipient->session->graph.lock);
  const sfu_remote_slot_t *slot = &recipient->session->graph.remote_slots.slots[recipient->remote_slot];
  bool valid = slot->state != SFU_REMOTE_SLOT_FREE && slot->assignment_generation == recipient->assignment_generation;
  pthread_mutex_unlock(&recipient->session->graph.lock);
  return valid;
}

static void flush_membership_events(sfu_signaling_server_t *s) {
  sfu_membership_event_t *event;
  while ((event = membership_queue_pop(&s->membership_queue)) != NULL) {
    if (event->kind == SFU_MEMBERSHIP_JOIN) {
      for (uint32_t r = 0; r < event->recipient_count; r++) {
        sfu_membership_recipient_t *recipient = &event->recipients[r];
        if (!membership_recipient_valid(recipient)) {
          continue;
        }
        if (recipient->send_snapshot) {
          size_t off = 0;
          int n = snprintf(s->scratch.json, SFU_SIGNALING_JSON_CAP,
                           "{\"type\":\"room_snapshot\",\"room\":\"%" PRIu64 "\",\"room_revision\":%" PRIu64
                           ",\"self_peer_id\":%u,\"participant_count\":%u,\"members\":[",
                           event->room_id, event->room_revision, event->subject_peer_id, event->participant_count);
          if (n <= 0 || (size_t)n >= SFU_SIGNALING_JSON_CAP) {
            continue;
          }
          off = (size_t)n;
          for (uint32_t i = 0; i < event->member_count; i++) {
            const sfu_membership_member_t *m = &event->members[i];
            n = snprintf(s->scratch.json + off, SFU_SIGNALING_JSON_CAP - off,
                         "%s{\"peer_id\":%u,\"user_id\":\"%" PRId64
                         "\",\"role\":\"%s\",\"is_mute\":%s,\"camera_requested\":%s,\"camera_active\":%s,"
                         "\"screen_requested\":%s,\"screen_active\":%s,\"ufrag\":\"%s\",\"mid_audio\":%u,\"mid_video\":%u,\"mid_screen\":%u,"
                         "\"slot\":%u,\"assignment_generation\":%" PRIu64 "}",
                         i ? "," : "", m->peer_id, m->user_id, m->is_audience ? "audience" : "speaker", m->is_mute ? "true" : "false",
                         m->camera_requested ? "true" : "false", m->camera_active ? "true" : "false", m->screen_requested ? "true" : "false",
                         m->screen_active ? "true" : "false", m->ufrag, m->mid_audio, m->mid_video, m->mid_screen, m->remote_slot, m->assignment_generation);
            if (n < 0 || (size_t)n >= SFU_SIGNALING_JSON_CAP - off) {
              off = 0;
              break;
            }
            off += (size_t)n;
          }
          if (off && off + 2 < SFU_SIGNALING_JSON_CAP) {
            s->scratch.json[off++] = ']';
            s->scratch.json[off++] = '}';
            s->scratch.json[off] = '\0';
            (void)sfu_ws_send_text(recipient->fd, s->scratch.json, off);
          }
        } else if (recipient->send_delta) {
          char message[640];
          int n = snprintf(message, sizeof(message),
                           "{\"type\":\"peer_joined\",\"room_revision\":%" PRIu64
                           ",\"participant_count\":%u,\"peer\":{"
                           "\"peer_id\":%u,\"user_id\":\"%" PRId64
                           "\",\"role\":\"%s\",\"is_mute\":%s,"
                           "\"camera_requested\":%s,\"camera_active\":%s,\"screen_requested\":%s,\"screen_active\":%s,"
                           "\"ufrag\":\"%s\",\"mid_audio\":%u,\"mid_video\":%u,\"mid_screen\":%u,\"slot\":%u,\"assignment_generation\":%" PRIu64 "}}",
                           event->room_revision, event->participant_count, event->subject_peer_id, event->subject_user_id,
                           event->subject_is_audience ? "audience" : "speaker", event->subject_is_mute ? "true" : "false",
                           event->subject_camera_requested ? "true" : "false", event->subject_camera_active ? "true" : "false",
                           event->subject_screen_requested ? "true" : "false", event->subject_screen_active ? "true" : "false", event->subject_ufrag,
                           recipient->mid_audio, recipient->mid_video, recipient->mid_screen, recipient->remote_slot, recipient->assignment_generation);
          if (n > 0 && (size_t)n < sizeof(message)) {
            (void)sfu_ws_send_text(recipient->fd, message, (size_t)n);
          }
        }
      }
    } else if (event->kind == SFU_MEMBERSHIP_LEAVE) {
      for (uint32_t r = 0; r < event->recipient_count; r++) {
        sfu_membership_recipient_t *recipient = &event->recipients[r];
        if (!membership_recipient_valid(recipient) || !recipient->send_delta) {
          continue;
        }
        char message[448];
        int n = snprintf(message, sizeof(message),
                         "{\"type\":\"peer_left\",\"room_revision\":%" PRIu64
                         ",\"participant_count\":%u,"
                         "\"ufrag\":\"%s\",\"user_id\":\"%" PRId64
                         "\",\"peer_id\":%u,"
                         "\"mid_audio\":%u,\"mid_video\":%u,\"mid_screen\":%u,\"slot\":%u,\"assignment_generation\":%" PRIu64 "}",
                         event->room_revision, event->participant_count, event->subject_ufrag, event->subject_user_id, event->subject_peer_id,
                         recipient->mid_audio, recipient->mid_video, recipient->mid_screen, recipient->remote_slot, recipient->assignment_generation);
        if (n > 0 && (size_t)n < sizeof(message)) {
          (void)sfu_ws_send_text(recipient->fd, message, (size_t)n);
        }
      }
    }
    for (uint32_t r = 0; r < event->recipient_count; r++) {
      sfu_membership_recipient_t *recipient = &event->recipients[r];
      if (recipient->renegotiate && membership_recipient_valid(recipient)) {
        sfu_signaling_trigger_peer_renegotiation(recipient->session);
      }
    }
    sfu_membership_event_release(event);
  }
}

static sfu_peer_session_t *renegotiation_queue_pop(sfu_renegotiation_queue_t *queue) {
  pthread_mutex_lock(&queue->lock);
  sfu_peer_session_t *session = NULL;
  sfu_renegotiation_fallback_node_t *fallback = queue->fallback_head;
  bool emergency_fallback = fallback == &queue->emergency_fallback;
  if (fallback) {
    queue->fallback_head = fallback->next;
    if (!queue->fallback_head) {
      queue->fallback_tail = NULL;
    }
    queue->fallback_count--;
    session = fallback->session;
    if (emergency_fallback) {
      queue->emergency_fallback_used = false;
      memset(&queue->emergency_fallback, 0, sizeof(queue->emergency_fallback));
    }
  } else if (queue->count > 0) {
    session = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % SFU_RENEGOTIATION_QUEUE_CAP;
    queue->count--;
  }
  if (session) {
    pthread_mutex_lock(&session->negotiation.lock);
    assert(session->negotiation.renegotiation_queued);
    session->negotiation.renegotiation_queued = false;
    pthread_mutex_unlock(&session->negotiation.lock);
  }
  pthread_mutex_unlock(&queue->lock);
  if (!emergency_fallback) {
    SFU_FREE(fallback);
  }
  return session;
}

static bool renegotiation_queue_enqueue_owned(sfu_renegotiation_queue_t *queue, sfu_peer_session_t *session) {
  sfu_renegotiation_fallback_node_t *fallback = NULL;
  sfu_peer_session_t *removed = NULL;
  bool consumed = false;
  bool used_fallback = false;

  pthread_mutex_lock(&queue->lock);
  if (queue->count >= SFU_RENEGOTIATION_QUEUE_CAP) {
    for (uint32_t offset = 0; offset < queue->count; offset++) {
      uint32_t index = (queue->head + offset) % SFU_RENEGOTIATION_QUEUE_CAP;
      sfu_peer_session_t *candidate = queue->items[index];
      pthread_mutex_lock(&candidate->negotiation.lock);
      bool stale = !sfu_session_accepts_work(candidate);
      if (stale) {
        candidate->negotiation.renegotiation_queued = false;
      }
      pthread_mutex_unlock(&candidate->negotiation.lock);
      if (!stale) {
        continue;
      }
      removed = candidate;
      for (uint32_t shift = offset; shift + 1 < queue->count; shift++) {
        uint32_t dst = (queue->head + shift) % SFU_RENEGOTIATION_QUEUE_CAP;
        uint32_t src = (queue->head + shift + 1) % SFU_RENEGOTIATION_QUEUE_CAP;
        queue->items[dst] = queue->items[src];
      }
      queue->tail = (queue->head + queue->count - 1) % SFU_RENEGOTIATION_QUEUE_CAP;
      queue->items[queue->tail] = NULL;
      queue->count--;
      break;
    }
  }

  pthread_mutex_lock(&session->negotiation.lock);
  bool eligible = session->state == SFU_SESSION_ESTABLISHED && !session->negotiation.offer_outstanding && session->negotiation.negotiation_needed &&
                  session->negotiation.renegotiation_pending && sfu_session_accepts_work(session);
  if (!session->negotiation.renegotiation_queued && eligible) {
    if (queue->count < SFU_RENEGOTIATION_QUEUE_CAP) {
      queue->items[queue->tail] = session;
      queue->tail = (queue->tail + 1) % SFU_RENEGOTIATION_QUEUE_CAP;
      queue->count++;
      consumed = true;
    } else {
      fallback = SFU_CALLOC(1, sizeof(*fallback));
      if (!fallback && !queue->emergency_fallback_used) {
        fallback = &queue->emergency_fallback;
        queue->emergency_fallback_used = true;
      }
      if (fallback) {
        fallback->session = session;
        if (queue->fallback_tail) {
          queue->fallback_tail->next = fallback;
        } else {
          queue->fallback_head = fallback;
        }
        queue->fallback_tail = fallback;
        queue->fallback_count++;
        consumed = true;
        used_fallback = true;
      }
    }
    if (consumed) {
      session->negotiation.renegotiation_queued = true;
    }
  }
  pthread_mutex_unlock(&session->negotiation.lock);
  pthread_mutex_unlock(&queue->lock);

  if (removed) {
    SFU_LOG_WARN("signaling: reclaimed stale queued peer %u for live peer %u", removed->peer_id, session->peer_id);
    sfu_session_release(removed);
  }
  if (used_fallback) {
    SFU_LOG_ERROR("signaling: all fixed renegotiation queue entries are live; retained fallback peer %u", session->peer_id);
    fallback = NULL;
  }
  SFU_FREE(fallback);
  return consumed;
}

static uint32_t renegotiation_queue_count(sfu_renegotiation_queue_t *queue) __attribute__((unused));
static uint32_t renegotiation_queue_count(sfu_renegotiation_queue_t *queue) {
  pthread_mutex_lock(&queue->lock);
  uint32_t count = queue->count + queue->fallback_count;
  pthread_mutex_unlock(&queue->lock);
  return count;
}

void sfu_signaling_renegotiation_test_server_init(sfu_signaling_server_t *s) {
  memset(s, 0, sizeof(*s));
  assert(pthread_mutex_init(&s->renegotiation_queue.lock, NULL) == 0);
  s->suppress_wake = true;
  pthread_mutex_lock(&g_signaling_producer_lock);
  assert(g_signaling_server == NULL && g_signaling_producers == 0);
  g_signaling_stopping = false;
  g_signaling_server = s;
  pthread_mutex_unlock(&g_signaling_producer_lock);
}

sfu_peer_session_t *sfu_signaling_renegotiation_test_pop(sfu_signaling_server_t *s) { return renegotiation_queue_pop(&s->renegotiation_queue); }

uint32_t sfu_signaling_renegotiation_test_count(sfu_signaling_server_t *s) { return renegotiation_queue_count(&s->renegotiation_queue); }

void sfu_signaling_renegotiation_test_server_stop(sfu_signaling_server_t *s) {
  pthread_mutex_lock(&g_signaling_producer_lock);
  g_signaling_stopping = true;
  if (g_signaling_server == s) {
    g_signaling_server = NULL;
  }
  while (g_signaling_producers != 0) {
    pthread_cond_wait(&g_signaling_producer_idle, &g_signaling_producer_lock);
  }
  pthread_mutex_unlock(&g_signaling_producer_lock);
  assert(renegotiation_queue_count(&s->renegotiation_queue) == 0);
  pthread_mutex_destroy(&s->renegotiation_queue.lock);
}

static void on_renegotiation_timer(uv_timer_t *timer);

static void arm_renegotiation_timer(sfu_signaling_server_t *s, uint64_t due_ms) {
  if (!s->renegotiation_timer_inited || due_ms == 0) {
    return;
  }
  uint64_t now_ms = sfu_now_ms();
  uint64_t delay_ms = due_ms > now_ms ? due_ms - now_ms : 1;
  uv_timer_start(&s->renegotiation_timer, on_renegotiation_timer, delay_ms, 0);
}

static void flush_pending_offers(sfu_signaling_server_t *s) {
  uint64_t earliest_due_ms = 0;
  uint32_t pending_count = renegotiation_queue_count(&s->renegotiation_queue);
  for (uint32_t processed = 0; processed < pending_count; processed++) {
    sfu_peer_session_t *session = renegotiation_queue_pop(&s->renegotiation_queue);
    if (!session) {
      break;
    }

    int fd = -1;
    uint64_t attempt_revision = 0;
    bool send = false;
    bool requeue = false;
    uint64_t now_ms = sfu_now_ms();
    pthread_mutex_lock(&session->negotiation.lock);
    if (s->renegotiation_timer_inited && session->negotiation.negotiation_due_ms > now_ms) {
      requeue = true;
    } else {
      session->negotiation.negotiation_needed = false;
      if (session->state == SFU_SESSION_ESTABLISHED && !session->negotiation.offer_outstanding && sfu_session_accepts_work(session) && session->fd >= 0 &&
          session->negotiation.desired_offer_revision > session->negotiation.offered_revision) {
        session->negotiation.offer_outstanding = true;
        session->negotiation.renegotiation_pending = false;
        attempt_revision = session->negotiation.desired_offer_revision;
        fd = session->fd;
        send = true;
      }
    }
    uint64_t due_ms = session->negotiation.negotiation_due_ms;
    pthread_mutex_unlock(&session->negotiation.lock);

    if (requeue) {
      if (renegotiation_queue_enqueue_owned(&s->renegotiation_queue, session)) {
        if (earliest_due_ms == 0 || due_ms < earliest_due_ms) {
          earliest_due_ms = due_ms;
        }
        continue;
      }
      SFU_LOG_WARN("signaling: debounced offer for peer %u was concurrently queued or became ineligible", session->peer_id);
      if (earliest_due_ms == 0 || due_ms < earliest_due_ms) {
        earliest_due_ms = due_ms;
      }
    } else {
      uint64_t manifest_generation = 0;
      if (send && build_and_send_offer(fd, session, s, &manifest_generation)) {
        pthread_mutex_lock(&session->negotiation.lock);
        session->negotiation.offered_revision = attempt_revision;
        session->negotiation.offer_generation = manifest_generation;
        session->negotiation.negotiation_retry_count = 0;
        pthread_mutex_unlock(&session->negotiation.lock);
        SFU_LOG_INFO("signaling: sent renegotiation offer ufrag=%s fd=%d peer_id=%u generation=%" PRIu64 " revision=%" PRIu64, session->cold->ufrag, fd,
                     session->peer_id, manifest_generation, attempt_revision);
      } else if (send) {
        pthread_mutex_lock(&session->negotiation.lock);
        if (session->negotiation.offer_outstanding) {
          session->negotiation.offer_outstanding = false;
          session->negotiation.renegotiation_pending = true;
          session->negotiation.negotiation_retry_count++;
          uint32_t shift = session->negotiation.negotiation_retry_count > 5 ? 5 : session->negotiation.negotiation_retry_count;
          uint64_t retry_ms = 15u << shift;
          if (retry_ms > SFU_RENEGOTIATION_RETRY_MAX_MS) {
            retry_ms = SFU_RENEGOTIATION_RETRY_MAX_MS;
          }
          session->negotiation.negotiation_due_ms = sfu_now_ms() + retry_ms;
          session->negotiation.negotiation_needed = true;
          due_ms = session->negotiation.negotiation_due_ms;
          requeue = true;
        }
        pthread_mutex_unlock(&session->negotiation.lock);
        SFU_LOG_WARN("signaling: failed renegotiation offer ufrag=%s fd=%d peer_id=%u", session->cold->ufrag, fd, session->peer_id);
        if (requeue && renegotiation_queue_enqueue_owned(&s->renegotiation_queue, session)) {
          if (earliest_due_ms == 0 || due_ms < earliest_due_ms) {
            earliest_due_ms = due_ms;
          }
          continue;
        }
        if (requeue) {
          SFU_LOG_WARN("signaling: retry offer for peer %u was concurrently queued or became ineligible", session->peer_id);
          if (earliest_due_ms == 0 || due_ms < earliest_due_ms) {
            earliest_due_ms = due_ms;
          }
        }
      }
    }
    sfu_session_release(session);
  }

  if (earliest_due_ms != 0) {
    arm_renegotiation_timer(s, earliest_due_ms);
  }
}

static void on_renegotiation_timer(uv_timer_t *timer) {
  sfu_signaling_server_t *s = timer->data;
  if (s && atomic_load(&s->running)) {
    flush_pending_offers(s);
  }
}

static void on_renegotiation_wake(uv_async_t *handle) {
  sfu_signaling_server_t *s = handle->data;
  if (s && atomic_load(&s->running)) {
    flush_membership_events(s);
    flush_media_state_events(s);
    flush_pending_offers(s);
  }
}

static void on_async_wake(uv_async_t *handle) { uv_stop(handle->loop); }

static void on_shutdown_walk(uv_handle_t *handle, void *arg) {
  if (uv_is_closing(handle)) {
    return;
  }

  sfu_signaling_server_t *s = (sfu_signaling_server_t *)arg;

  if (s->renegotiation_timer_inited && handle == (uv_handle_t *)&s->renegotiation_timer) {
    uv_timer_stop(&s->renegotiation_timer);
    uv_close(handle, NULL);
    return;
  }
  if (handle->type == UV_POLL && handle->data != NULL && handle->data != s) {
    disconnect_client((sfu_client_conn_t *)handle->data, SFU_DISCONNECT_GOING_AWAY);
    return;
  }
  if (handle->type == UV_TIMER && handle->data != NULL && handle->data != s) {
    disconnect_client((sfu_client_conn_t *)handle->data, SFU_DISCONNECT_GOING_AWAY);
    return;
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
  if (uv_timer_init(&loop, &s->renegotiation_timer) == 0) {
    s->renegotiation_timer.data = s;
    s->renegotiation_timer_inited = true;
  } else {
    s->renegotiation_timer_inited = false;
    SFU_LOG_ERROR("signaling: failed to initialize renegotiation timer");
  }

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
  if (pthread_cond_init(&s->membership_queue.not_full, NULL) != 0) {
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }
  s->membership_queue.accepting = true;
  if (!signaling_scratch_init(&s->scratch)) {
    SFU_LOG_ERROR("signaling: failed to allocate loop scratch workspace");
    pthread_cond_destroy(&s->membership_queue.not_full);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }

  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->listen_fd < 0) {
    SFU_LOG_ERROR("signaling: socket() failed");
    signaling_scratch_destroy(&s->scratch);
    pthread_cond_destroy(&s->membership_queue.not_full);
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
    pthread_cond_destroy(&s->membership_queue.not_full);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }
  if (listen(s->listen_fd, 16) < 0) {
    SFU_LOG_ERROR("signaling: listen() failed");
    close(s->listen_fd);
    signaling_scratch_destroy(&s->scratch);
    pthread_cond_destroy(&s->membership_queue.not_full);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }

  atomic_store(&s->running, true);
  if (pthread_create(&s->thread, NULL, signaling_loop_main, s) != 0) {
    SFU_LOG_ERROR("signaling: failed to spawn accept loop thread");
    close(s->listen_fd);
    signaling_scratch_destroy(&s->scratch);
    pthread_cond_destroy(&s->membership_queue.not_full);
    pthread_mutex_destroy(&s->membership_queue.lock);
    pthread_mutex_destroy(&s->renegotiation_queue.lock);
    return -1;
  }

  pthread_mutex_lock(&g_signaling_producer_lock);
  g_signaling_stopping = false;
  g_signaling_server = s;
  pthread_mutex_unlock(&g_signaling_producer_lock);

  SFU_LOG_INFO("signaling server listening on ws://0.0.0.0:%u", listen_port);
  return 0;
}

void sfu_signaling_server_stop(sfu_signaling_server_t *s) {
  if (!s || !atomic_load(&s->running)) {
    return;
  }

  pthread_mutex_lock(&g_signaling_producer_lock);
  g_signaling_stopping = true;
  if (g_signaling_server == s) {
    g_signaling_server = NULL;
  }
  pthread_mutex_unlock(&g_signaling_producer_lock);

  pthread_mutex_lock(&s->membership_queue.lock);
  s->membership_queue.accepting = false;
  pthread_cond_broadcast(&s->membership_queue.not_full);
  pthread_mutex_unlock(&s->membership_queue.lock);

  atomic_store(&s->running, false);
  uv_async_send(&s->async_waker);

  pthread_mutex_lock(&g_signaling_producer_lock);
  while (g_signaling_producers != 0) {
    pthread_cond_wait(&g_signaling_producer_idle, &g_signaling_producer_lock);
  }
  pthread_mutex_unlock(&g_signaling_producer_lock);

  pthread_join(s->thread, NULL);
  close(s->listen_fd);

  sfu_peer_session_t *queued;
  while ((queued = renegotiation_queue_pop(&s->renegotiation_queue)) != NULL) {
    sfu_session_release(queued);
  }
  sfu_membership_event_t *membership_event;
  while ((membership_event = membership_queue_pop(&s->membership_queue)) != NULL) {
    sfu_membership_event_release(membership_event);
  }
  while ((queued = media_update_queue_pop(&s->membership_queue)) != NULL) {
    atomic_store_explicit(&queued->media.media_update_queued, false, memory_order_release);
    sfu_session_release(queued);
  }
  signaling_scratch_destroy(&s->scratch);
  pthread_cond_destroy(&s->membership_queue.not_full);
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
