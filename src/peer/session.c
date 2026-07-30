#include "peer/session.h"
#include <string.h>
#include "room/room_media_graph.h"
#include "runtime/routing_context.h"
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
      sfu_dtls_conn_destroy(&t->sessions[i]->dtls);
    }
    if (t->sessions[i]->receivers) {
      for (uint32_t j = 0; j < t->sessions[i]->receiver_capacity; j++) {
        SFU_FREE(t->sessions[i]->receivers[j]);
      }
      SFU_FREE(t->sessions[i]->receivers);
    }
    SFU_FREE(t->sessions[i]);
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
  if (idx >= ctx->t->count) return false;
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->active && s->addr_len == ctx->addr_len && memcmp(&s->addr, ctx->addr, ctx->addr_len) == 0;
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

  uint32_t hash = fnv1a(&session->addr, session->addr_len);
  addr_match_ctx_t ctx = {t, &session->addr, session->addr_len};
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

    if (session->active && addr_equal(&session->addr, session->addr_len, addr, addr_len)) {
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

  if (!addr || addr_len > sizeof(s->addr)) {
    SFU_FREE(s); t->sessions[index] = NULL; t->count--;
    pthread_mutex_unlock(&t->lock); return NULL;
  }
  memcpy(&s->addr, addr, addr_len);
  s->addr_len = addr_len;
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

  sfu_session_table_index_addr(t, s);

  if (sfu_dtls_conn_init(&s->dtls, t->dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS connection for new peer session");

    SFU_FREE(s->receivers);

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
  if (idx >= ctx->t->count) return false;
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->active && s->addr_len == ctx->addr_len && memcmp(&s->addr, ctx->addr, ctx->addr_len) == 0;
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
  if (idx >= ctx->t->count) return false;
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->active && s->ufrag[0] != '\0' && strcmp(s->ufrag, ctx->ufrag) == 0;
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
  for (uint32_t i = 0; i < SFU_SESSION_ADDR_HASH_SLOTS; i++) if (t->addr_index[i].index < t->count && t->sessions[t->addr_index[i].index] == s) t->addr_index[i].index = SFU_HASH_DELETED;
  for (uint32_t i = 0; i < SFU_SESSION_UFRAG_HASH_SLOTS; i++) if (t->ufrag_index[i].index < t->count && t->sessions[t->ufrag_index[i].index] == s) t->ufrag_index[i].index = SFU_HASH_DELETED;
  if (s->active) {
    if (s->state == SFU_SESSION_ESTABLISHED) {
      sfu_srtp_ctx_destroy(&s->srtp);
    }
    sfu_dtls_conn_destroy(&s->dtls);

    if (s->receivers) {
      for (uint32_t i = 0; i < s->receiver_capacity; i++) {
        SFU_FREE(s->receivers[i]);
      }
      SFU_FREE(s->receivers);
      s->receivers = NULL;
      s->receiver_capacity = 0;
    }

    s->active = false;
    s->state = SFU_SESSION_FAILED;
  }
  pthread_mutex_unlock(&t->lock);
}

void sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!addr || addr_len > sizeof(s->addr)) return;
  pthread_mutex_lock(&t->lock);
  for (uint32_t i = 0; i < SFU_SESSION_ADDR_HASH_SLOTS; i++) {
    uint32_t idx = t->addr_index[i].index;
    if (idx < t->count && t->sessions[idx] == s) t->addr_index[i].index = SFU_HASH_DELETED;
  }
  memcpy(&s->addr, addr, addr_len);
  s->addr_len = addr_len;
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

  uint32_t hash = fnv1a(session->ufrag, strlen(session->ufrag));
  ufrag_match_ctx_t ctx = {t, session->ufrag};
  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches, &ctx, true);
  if (slot != SFU_HASH_EMPTY) {
    t->ufrag_index[slot].hash = hash;
    t->ufrag_index[slot].index = idx;
  }

  pthread_mutex_unlock(&t->lock);
}
