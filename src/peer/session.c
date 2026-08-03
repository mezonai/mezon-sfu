#include "peer/session.h"
#include <string.h>
#include "congestion/gcc.h"
#include "congestion/twcc_history.h"
#include "room/room_media_graph.h"
#include "rtcp/rtcp_kf.h"
#include "rtp/rtx.h"
#include "runtime/routing_context.h"
#include "runtime/scheduler.h"
#include "runtime/worker.h"
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "util/alloc.h"
#include "util/log.h"

typedef struct {
  sfu_session_table_t *t;
  const struct sockaddr_storage *addr;
  socklen_t addr_len;
} addr_match_ctx_t;

typedef struct {
  sfu_session_table_t *t;
  const char *ufrag;
} ufrag_match_ctx_t;

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx) {
  memset(t, 0, sizeof(*t));
  t->capacity = SFU_SESSION_TABLE_MAX;
  t->sessions = SFU_CALLOC(t->capacity, sizeof(*t->sessions));

  if (!t->sessions) {
    return -1;
  }

  for (int i = 0; i < SFU_SESSION_ADDR_HASH_SLOTS; i++) {
    t->addr_index[i].index = SFU_HASH_EMPTY;
  }
  for (int i = 0; i < SFU_SESSION_UFRAG_HASH_SLOTS; i++) {
    t->ufrag_index[i].index = SFU_HASH_EMPTY;
  }

  t->dtls_ctx = dtls_ctx;

  if (pthread_mutex_init(&t->lock, NULL) != 0) {
    SFU_FREE(t->sessions);
    t->sessions = NULL;
    t->capacity = 0;
    return -1;
  }

  return 0;
}

static uint32_t addr_probe(sfu_hash_slot_t *table, uint32_t cap, uint32_t hash, bool (*match)(uint32_t idx, void *ctx), void *ctx, bool for_insert) {
  uint32_t start = hash & (cap - 1);
  int32_t first_deleted = -1;
  for (uint32_t probe = 0; probe < cap; probe++) {
    uint32_t slot = (start + probe) & (cap - 1);
    if (table[slot].index == SFU_HASH_EMPTY) {
      return for_insert ? (first_deleted >= 0 ? (uint32_t)first_deleted : slot) : SFU_HASH_EMPTY;
    }
    if (table[slot].index == SFU_HASH_DELETED) {
      if (first_deleted < 0) {
        first_deleted = (int32_t)slot;
      }
      continue;
    }
    if (table[slot].hash == hash && match(table[slot].index, ctx)) {
      return slot;
    }
  }
  return for_insert && first_deleted >= 0 ? (uint32_t)first_deleted : SFU_HASH_EMPTY;
}

void sfu_session_table_destroy(sfu_session_table_t *t) {
  pthread_mutex_lock(&t->lock);

  for (uint32_t i = 0; i < t->count; i++) {
    if (!t->sessions[i]) {
      continue;
    }

    if (t->sessions[i]->active) {
      if (t->sessions[i]->state == SFU_SESSION_ESTABLISHED) {
        sfu_srtp_ctx_destroy(&t->sessions[i]->srtp);
      }
      sfu_dtls_conn_destroy(&t->sessions[i]->cold->dtls);
    }
    if (t->sessions[i]->receivers) {
      for (uint32_t j = 0; j < t->sessions[i]->receiver_capacity; j++) {
        SFU_FREE(t->sessions[i]->receivers[j]);
      }
      SFU_FREE(t->sessions[i]->receivers);
    }
    sfu_peer_session_t *s = t->sessions[i];
    if (!s) {
      continue;
    }

    SFU_FREE(s->receivers);

    /* Free RTX cache if allocated */
    if (s->rtx_cache) {
      sfu_rtx_cache_destroy(s->rtx_cache);
      SFU_FREE(s->rtx_cache);
    }

    /* Free other context pointers if allocated */
    if (s->gcc_ctx) {
      SFU_FREE(s->gcc_ctx);
    }
    if (s->twcc_history) {
      SFU_FREE(s->twcc_history);
    }
    if (s->scheduler) {
      SFU_FREE(s->scheduler);
    }

    SFU_FREE(s->cold);
    SFU_FREE(s);
  }

  SFU_FREE(t->sessions);
  t->sessions = NULL;
  t->count = 0;
  t->capacity = 0;

  pthread_mutex_unlock(&t->lock);
  pthread_mutex_destroy(&t->lock);
}

static bool addr_equal(const struct sockaddr_storage *a, socklen_t a_len, const struct sockaddr_storage *b, socklen_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  return memcmp(a, b, a_len) == 0;
}

static bool addr_matches_direct(uint32_t idx, void *ctx_) {
  addr_match_ctx_t *ctx = ctx_;
  if (idx >= ctx->t->count) {
    return false;
  }
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->active && s->cold->addr_len == ctx->addr_len && memcmp(&s->cold->addr, ctx->addr, ctx->addr_len) == 0;
}

void sfu_session_table_index_addr(sfu_session_table_t *t, sfu_peer_session_t *session) {
  uint32_t idx = UINT32_MAX;
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i] == session) {
      idx = i;
      break;
    }
  }
  if (idx == UINT32_MAX) {
    return;
  }

  uint32_t hash = fnv1a(&session->cold->addr, session->cold->addr_len);
  addr_match_ctx_t ctx = {t, &session->cold->addr, session->cold->addr_len};
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, true);
  if (slot != SFU_HASH_EMPTY) {
    t->addr_index[slot].hash = hash;
    t->addr_index[slot].index = idx;
  }
}

sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  pthread_mutex_lock(&t->lock);

  for (uint32_t i = 0; i < t->count; i++) {
    sfu_peer_session_t *session = t->sessions[i];

    if (!session) {
      continue;
    }

    if (session->active && addr_equal(&session->cold->addr, session->cold->addr_len, addr, addr_len)) {
      pthread_mutex_unlock(&t->lock);
      return session;
    }
  }

  uint32_t index;

  if (t->count < t->capacity) {
    index = t->count++;
    t->sessions[index] = SFU_CALLOC(1, sizeof(sfu_peer_session_t));
    if (!t->sessions[index]) {
      t->count--;
      pthread_mutex_unlock(&t->lock);
      return NULL;
    }
  } else {
    SFU_LOG_WARN("session table full (%u), rejecting new peer", t->capacity);
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  sfu_peer_session_t *s = t->sessions[index];

  memset(s, 0, sizeof(*s));

  s->cold = SFU_CALLOC(1, sizeof(sfu_peer_session_cold_t));
  if (!s->cold) {
    SFU_FREE(s);
    t->sessions[index] = NULL;
    t->count--;
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  if (!addr || addr_len > sizeof(s->cold->addr)) {
    SFU_FREE(s->cold);
    SFU_FREE(s);
    t->sessions[index] = NULL;
    t->count--;
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  memcpy(&s->cold->addr, addr, addr_len);
  s->cold->addr_len = addr_len;
  s->active = true;
  s->state = SFU_SESSION_NEW;
  s->worker_id = UINT16_MAX;

  for (int i = 0; i < 128; i++) {
    s->pt_map[i] = (uint8_t)i;
  }

  s->uplink_audio.owner = s;
  s->uplink_video.owner = s;
  s->screen.owner = s;

  s->receiver_capacity = 0;
  s->receivers = NULL;

  s->next_remote_mid = 2;

  sfu_session_table_index_addr(t, s);

  s->gcc_ctx = SFU_CALLOC(1, sizeof(gcc_bwe_context_t));
  if (s->gcc_ctx) {
    gcc_bwe_init(s->gcc_ctx, 300000, 50000, 5000000);
  }

  s->twcc_history = SFU_CALLOC(1, sizeof(sfu_twcc_history_t));

  s->scheduler = SFU_CALLOC(1, sizeof(sfu_subscriber_scheduler_t));
  if (s->scheduler) {
    sfu_subscriber_scheduler_init(s->scheduler, 0);
  }

  s->rtx_cache = SFU_CALLOC(1, sizeof(sfu_rtx_cache_t));
  if (s->rtx_cache) {
    sfu_rtx_cache_init(s->rtx_cache);
  }

  if (sfu_dtls_conn_init(&s->cold->dtls, t->dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS connection for new peer session");

    SFU_FREE(s->receivers);

    if (s->cold) {
      SFU_FREE(s->cold);
    }

    if (s->gcc_ctx) {
      SFU_FREE(s->gcc_ctx);
    }
    if (s->twcc_history) {
      SFU_FREE(s->twcc_history);
    }
    if (s->scheduler) {
      SFU_FREE(s->scheduler);
    }
    if (s->rtx_cache) {
      sfu_rtx_cache_destroy(s->rtx_cache);
      SFU_FREE(s->rtx_cache);
    }

    SFU_FREE(s);
    t->sessions[index] = NULL;

    if (index + 1 == t->count) {
      t->count--;
    }

    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  pthread_mutex_unlock(&t->lock);
  return s;
}

static bool addr_matches(uint32_t idx, void *ctx_) {
  addr_match_ctx_t *ctx = ctx_;
  if (idx >= ctx->t->count) {
    return false;
  }
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->active && s->cold->addr_len == ctx->addr_len && memcmp(&s->cold->addr, ctx->addr, ctx->addr_len) == 0;
}

sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  uint32_t hash = fnv1a(addr, addr_len);
  addr_match_ctx_t ctx = {t, addr, addr_len};
  pthread_mutex_lock(&t->lock);
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches, &ctx, false);
  sfu_peer_session_t *result = (slot != SFU_HASH_EMPTY && t->addr_index[slot].index != SFU_HASH_EMPTY) ? t->sessions[t->addr_index[slot].index] : NULL;
  pthread_mutex_unlock(&t->lock);
  return result;
}

static bool ufrag_matches(uint32_t idx, void *ctx_) {
  ufrag_match_ctx_t *ctx = ctx_;
  if (idx >= ctx->t->count) {
    return false;
  }
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->active && s->cold->ufrag[0] != '\0' && strcmp(s->cold->ufrag, ctx->ufrag) == 0;
}

sfu_peer_session_t *sfu_session_table_find_by_ufrag(sfu_session_table_t *t, const char *ufrag) {
  if (!ufrag || ufrag[0] == '\0') {
    return NULL;
  }

  uint32_t hash = fnv1a(ufrag, strlen(ufrag));
  ufrag_match_ctx_t ctx = {t, ufrag};

  pthread_mutex_lock(&t->lock);
  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches, &ctx, false);
  sfu_peer_session_t *result = (slot != SFU_HASH_EMPTY && t->ufrag_index[slot].index != SFU_HASH_EMPTY) ? t->sessions[t->ufrag_index[slot].index] : NULL;
  pthread_mutex_unlock(&t->lock);
  return result;
}

void sfu_session_table_remove(sfu_session_table_t *t, sfu_peer_session_t *s) {
  if (s->room) {
    room_remove_peer((sfu_room_t *)s->room, s);
  }

  pthread_mutex_lock(&t->lock);
  if (s->cold->addr_len > 0) {
    uint32_t hash = fnv1a(&s->cold->addr, s->cold->addr_len);
    addr_match_ctx_t ctx = {t, &s->cold->addr, s->cold->addr_len};
    uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, false);
    if (slot != SFU_HASH_EMPTY) {
      t->addr_index[slot].index = SFU_HASH_DELETED;
    }
  }
  if (s->cold->ufrag[0] != '\0') {
    uint32_t hash = fnv1a(s->cold->ufrag, strlen(s->cold->ufrag));
    ufrag_match_ctx_t ctx = {t, s->cold->ufrag};
    uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches, &ctx, false);
    if (slot != SFU_HASH_EMPTY) {
      t->ufrag_index[slot].index = SFU_HASH_DELETED;
    }
  }
  if (s->active) {
    if (s->state == SFU_SESSION_ESTABLISHED) {
      sfu_srtp_ctx_destroy(&s->srtp);
    }
    sfu_dtls_conn_destroy(&s->cold->dtls);

    if (s->receivers) {
      for (uint32_t i = 0; i < s->receiver_capacity; i++) {
        SFU_FREE(s->receivers[i]);
      }
      SFU_FREE(s->receivers);
      s->receivers = NULL;
      s->receiver_capacity = 0;
    }

    /* Free RTX cache if allocated */
    if (s->rtx_cache) {
      sfu_rtx_cache_destroy(s->rtx_cache);
      SFU_FREE(s->rtx_cache);
      s->rtx_cache = NULL;
    }

    /* Free other context pointers if allocated */
    if (s->gcc_ctx) {
      SFU_FREE(s->gcc_ctx);
      s->gcc_ctx = NULL;
    }
    if (s->twcc_history) {
      SFU_FREE(s->twcc_history);
      s->twcc_history = NULL;
    }
    if (s->scheduler) {
      SFU_FREE(s->scheduler);
      s->scheduler = NULL;
    }

    s->active = false;
    s->state = SFU_SESSION_FAILED;
  }
  pthread_mutex_unlock(&t->lock);
}

void sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!addr || addr_len > sizeof(s->cold->addr)) {
    return;
  }
  pthread_mutex_lock(&t->lock);
  if (s->cold->addr_len > 0) {
    uint32_t hash = fnv1a(&s->cold->addr, s->cold->addr_len);
    addr_match_ctx_t ctx = {t, &s->cold->addr, s->cold->addr_len};
    uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, false);
    if (slot != SFU_HASH_EMPTY) {
      t->addr_index[slot].index = SFU_HASH_DELETED;
    }
  }
  memcpy(&s->cold->addr, addr, addr_len);
  s->cold->addr_len = addr_len;
  sfu_session_table_index_addr(t, s);
  pthread_mutex_unlock(&t->lock);
}

void sfu_session_table_index_ufrag(sfu_session_table_t *t, sfu_peer_session_t *session) {
  pthread_mutex_lock(&t->lock);

  uint32_t idx = UINT32_MAX;
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i] == session) {
      idx = i;
      break;
    }
  }
  if (idx == UINT32_MAX) {
    pthread_mutex_unlock(&t->lock);
    return;
  }

  uint32_t hash = fnv1a(session->cold->ufrag, strlen(session->cold->ufrag));
  ufrag_match_ctx_t ctx = {t, session->cold->ufrag};
  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches, &ctx, true);
  if (slot != SFU_HASH_EMPTY) {
    t->ufrag_index[slot].hash = hash;
    t->ufrag_index[slot].index = idx;
  }

  pthread_mutex_unlock(&t->lock);
}

void sfu_session_request_keyframe(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir) {
  sfu_packet_t *rtcp_pkt = sfu_packet_pool_alloc(w->pp);
  if (!rtcp_pkt) {
    return;
  }

  int rtcp_len = 0;

  // The SFU's identifier in the RTCP packet.
  // Safely hardcoded to 1 since we are just an intermediate router.
  uint32_t sfu_sender_ssrc = 1;

  // The publisher's media SSRC that we want a keyframe for
  uint32_t media_ssrc = publisher->uplink_video.ssrc;

  if (use_fir) {
    rtcp_len = sfu_rtcp_build_fir(sfu_sender_ssrc, media_ssrc, &publisher->fir_seq, rtcp_pkt->data, rtcp_pkt->cap);
  } else {
    rtcp_len = sfu_rtcp_build_pli(sfu_sender_ssrc, media_ssrc, rtcp_pkt->data, rtcp_pkt->cap);
  }

  if (rtcp_len > 0) {
    if (sfu_srtp_protect_rtcp(&publisher->srtp, rtcp_pkt->data, &rtcp_len, rtcp_pkt->cap)) {
      rtcp_pkt->len = (uint32_t)rtcp_len;

      // Send the RTCP packet back to the publisher
      sfu_ring_queue_send_zc(&w->send_ring, rtcp_pkt, (const struct sockaddr *)&publisher->cold->addr, publisher->cold->addr_len);
    } else {
      SFU_LOG_WARN("Failed to SRTP protect keyframe request for peer %u", publisher->peer_id);
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtcp_pkt);
}
