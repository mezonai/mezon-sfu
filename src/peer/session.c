#include "peer/session.h"
#include <assert.h>
#include <inttypes.h>
#include <sched.h>
#include <string.h>
#include "congestion/gcc.h"
#include "congestion/pacer.h"
#include "congestion/twcc_feedback.h"
#include "congestion/twcc_history.h"
#include "room/room_media_graph.h"
#include "rtcp/rtcp_kf.h"
#include "rtp/rtx.h"
#include "runtime/routing_context.h"
#include "runtime/scheduler.h"
#include "runtime/timer.h"
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

#define SFU_SESSION_KF_THROTTLE_MS 1000
#define SFU_SNAPSHOT_HAZARD_SLOTS 256

static _Atomic(sfu_receiver_snapshot_t *) snapshot_hazards[SFU_SNAPSHOT_HAZARD_SLOTS];
static _Atomic bool snapshot_hazard_claimed[SFU_SNAPSHOT_HAZARD_SLOTS];
static pthread_key_t snapshot_hazard_key;
static pthread_once_t snapshot_hazard_key_once = PTHREAD_ONCE_INIT;

static void snapshot_hazard_thread_exit(void *value) {
  if (!value) {
    return;
  }
  uint32_t slot = (uint32_t)((uintptr_t)value - 1u);
  atomic_store_explicit(&snapshot_hazards[slot], NULL, memory_order_seq_cst);
  atomic_store_explicit(&snapshot_hazard_claimed[slot], false, memory_order_release);
}

static void snapshot_hazard_make_key(void) { (void)pthread_key_create(&snapshot_hazard_key, snapshot_hazard_thread_exit); }

static _Atomic(sfu_receiver_snapshot_t *) *snapshot_hazard_for_thread(void) {
  pthread_once(&snapshot_hazard_key_once, snapshot_hazard_make_key);
  void *value = pthread_getspecific(snapshot_hazard_key);
  if (value) {
    uint32_t slot = (uint32_t)((uintptr_t)value - 1u);
    return &snapshot_hazards[slot];
  }
  for (uint32_t slot = 0; slot < SFU_SNAPSHOT_HAZARD_SLOTS; slot++) {
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(&snapshot_hazard_claimed[slot], &expected, true, memory_order_acq_rel, memory_order_relaxed)) {
      if (pthread_setspecific(snapshot_hazard_key, (void *)(uintptr_t)(slot + 1u)) != 0) {
        atomic_store_explicit(&snapshot_hazard_claimed[slot], false, memory_order_release);
        return NULL;
      }
      return &snapshot_hazards[slot];
    }
  }
  return NULL;
}

static sfu_receiver_snapshot_t *snapshot_acquire(const sfu_peer_session_t *owner, const _Atomic(sfu_receiver_snapshot_t *) *source) {
  _Atomic(sfu_receiver_snapshot_t *) *hazard = snapshot_hazard_for_thread();
  if (!hazard) {
    pthread_mutex_lock((pthread_mutex_t *)&owner->snapshot_lock);
    sfu_receiver_snapshot_t *snap = atomic_load_explicit(source, memory_order_acquire);
    if (snap) {
      atomic_fetch_add_explicit(&snap->refcount, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock((pthread_mutex_t *)&owner->snapshot_lock);
    return snap;
  }
  for (;;) {
    sfu_receiver_snapshot_t *snap = atomic_load_explicit(source, memory_order_seq_cst);
    atomic_store_explicit(hazard, snap, memory_order_seq_cst);
    if (atomic_load_explicit(source, memory_order_seq_cst) != snap) {
      atomic_store_explicit(hazard, NULL, memory_order_seq_cst);
      continue;
    }
    if (snap) {
      atomic_fetch_add_explicit(&snap->refcount, 1, memory_order_relaxed);
    }
    atomic_store_explicit(hazard, NULL, memory_order_seq_cst);
    return snap;
  }
}

static void snapshot_wait_unhazarded(sfu_receiver_snapshot_t *snap) {
  if (!snap) {
    return;
  }
  for (;;) {
    bool found = false;
    for (uint32_t i = 0; i < SFU_SNAPSHOT_HAZARD_SLOTS; i++) {
      if (atomic_load_explicit(&snapshot_hazards[i], memory_order_seq_cst) == snap) {
        found = true;
        break;
      }
    }
    if (!found) {
      return;
    }
    sched_yield();
  }
}

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

  pthread_rwlockattr_t rwattr;
  if (pthread_rwlockattr_init(&rwattr) != 0 ||
      pthread_rwlockattr_setkind_np(&rwattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) != 0) {
    SFU_FREE(t->sessions);
    t->sessions = NULL;
    t->capacity = 0;
    return -1;
  }
  int rwlock_rc = pthread_rwlock_init(&t->lock, &rwattr);
  pthread_rwlockattr_destroy(&rwattr);
  if (rwlock_rc != 0) {
    SFU_FREE(t->sessions);
    t->sessions = NULL;
    t->capacity = 0;
    return -1;
  }
  if (pthread_mutex_init(&t->ice_lock, NULL) != 0) {
    pthread_rwlock_destroy(&t->lock);
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

sfu_receiver_snapshot_t *sfu_session_subscriptions_acquire(const sfu_peer_session_t *s) {
  return s ? snapshot_acquire(s, &s->receivers) : NULL;
}

void sfu_subscriptions_snapshot_release(sfu_receiver_snapshot_t *snap) {
  if (!snap) {
    return;
  }
  uint32_t prev = atomic_fetch_sub_explicit(&snap->refcount, 1, memory_order_acq_rel);
  assert(prev != 0 && "snapshot refcount underflow");
  if (prev == 1) {
    for (uint32_t i = 0; i < snap->count; i++) {
      sfu_session_release(snap->entries[i].subscriber);
    }
    SFU_FREE(snap);
  }
}

void sfu_session_publish_receivers(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) {
  pthread_mutex_lock(&owner->snapshot_lock);
  sfu_receiver_snapshot_t *old = atomic_load_explicit(&owner->receivers, memory_order_acquire);
  atomic_store_explicit(&owner->receivers, new_snap, memory_order_release);
  pthread_mutex_unlock(&owner->snapshot_lock);
  snapshot_wait_unhazarded(old);
  sfu_subscriptions_snapshot_release(old);
}

sfu_receiver_snapshot_t *sfu_session_fanout_targets_acquire(const sfu_peer_session_t *s) {
  return s ? snapshot_acquire(s, &s->fanout_targets) : NULL;
}

void sfu_session_publish_fanout_targets(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) {
  pthread_mutex_lock(&owner->snapshot_lock);
  sfu_receiver_snapshot_t *old = atomic_load_explicit(&owner->fanout_targets, memory_order_acquire);
  atomic_store_explicit(&owner->fanout_targets, new_snap, memory_order_release);
  pthread_mutex_unlock(&owner->snapshot_lock);
  snapshot_wait_unhazarded(old);
  sfu_subscriptions_snapshot_release(old);
}

static void sfu_session_free_resources(sfu_peer_session_t *s) {
  if (!s) {
    return;
  }

  if (s->active) {
    if (s->state == SFU_SESSION_ESTABLISHED) {
      sfu_srtp_ctx_destroy(&s->srtp);
    }
    if (s->cold) {
      sfu_dtls_conn_destroy(&s->cold->dtls);
    }
  }

  pthread_mutex_lock(&s->snapshot_lock);
  sfu_receiver_snapshot_t *snap = atomic_load_explicit(&s->receivers, memory_order_acquire);
  atomic_store_explicit(&s->receivers, NULL, memory_order_release);
  pthread_mutex_unlock(&s->snapshot_lock);
  snapshot_wait_unhazarded(snap);
  sfu_subscriptions_snapshot_release(snap);

  pthread_mutex_lock(&s->snapshot_lock);
  snap = atomic_load_explicit(&s->fanout_targets, memory_order_acquire);
  atomic_store_explicit(&s->fanout_targets, NULL, memory_order_release);
  pthread_mutex_unlock(&s->snapshot_lock);
  snapshot_wait_unhazarded(snap);
  sfu_subscriptions_snapshot_release(snap);

  if (s->rtx_cache) {
    sfu_rtx_cache_destroy(s->rtx_cache);
    SFU_FREE(s->rtx_cache);
    s->rtx_cache = NULL;
  }
  if (s->gcc_ctx) {
    SFU_FREE(s->gcc_ctx);
    s->gcc_ctx = NULL;
  }
  if (s->twcc_history) {
    SFU_FREE(s->twcc_history);
    s->twcc_history = NULL;
  }
  if (s->twcc_recv) {
    SFU_FREE(s->twcc_recv);
    s->twcc_recv = NULL;
  }
  if (s->schedulers) {
    SFU_FREE(s->schedulers);
    s->schedulers = NULL;
  }
  if (s->cold) {
    SFU_FREE(s->cold);
    s->cold = NULL;
  }
  pthread_mutex_destroy(&s->ingress_lock);
  pthread_mutex_destroy(&s->snapshot_lock);
  pthread_mutex_destroy(&s->media_lock);
  pthread_mutex_destroy(&s->negotiation_lock);
  pthread_mutex_destroy(&s->answer_lock);
}

void sfu_session_release(sfu_peer_session_t *s) {
  if (!s) {
    return;
  }
  if (atomic_fetch_sub_explicit(&s->refcount, 1, memory_order_acq_rel) == 1) {
    sfu_session_free_resources(s);
    SFU_FREE(s);
  }
}

static bool addr_equal(const struct sockaddr_storage *a, socklen_t a_len, const struct sockaddr_storage *b, socklen_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  return memcmp(a, b, a_len) == 0;
}

static uint32_t table_member_index(const sfu_session_table_t *t, const sfu_peer_session_t *session) {
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i] == session) {
      return i;
    }
  }
  return UINT32_MAX;
}

static bool addr_matches_direct(uint32_t idx, void *ctx_) {
  addr_match_ctx_t *ctx = ctx_;
  if (idx >= ctx->t->count) {
    return false;
  }
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->cold->addr_len == ctx->addr_len && memcmp(&s->cold->addr, ctx->addr, ctx->addr_len) == 0;
}

static bool ufrag_matches_direct(uint32_t idx, void *ctx_) {
  ufrag_match_ctx_t *ctx = ctx_;
  if (idx >= ctx->t->count) {
    return false;
  }
  sfu_peer_session_t *s = ctx->t->sessions[idx];
  return s && s->cold->ufrag[0] != '\0' && strcmp(s->cold->ufrag, ctx->ufrag) == 0;
}

static void table_remove_addr_hash(sfu_session_table_t *t, sfu_peer_session_t *s, uint32_t idx) {
  if (s->cold->addr_len == 0) {
    return;
  }
  uint32_t hash = fnv1a(&s->cold->addr, s->cold->addr_len);
  addr_match_ctx_t ctx = {t, &s->cold->addr, s->cold->addr_len};
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, false);
  if (slot != SFU_HASH_EMPTY && t->addr_index[slot].index == idx) {
    t->addr_index[slot].index = SFU_HASH_DELETED;
  }
}

static void table_remove_ufrag_hash(sfu_session_table_t *t, sfu_peer_session_t *s, uint32_t idx) {
  if (s->cold->ufrag[0] == '\0') {
    return;
  }
  uint32_t hash = fnv1a(s->cold->ufrag, strlen(s->cold->ufrag));
  ufrag_match_ctx_t ctx = {t, s->cold->ufrag};
  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches_direct, &ctx, false);
  if (slot != SFU_HASH_EMPTY && t->ufrag_index[slot].index == idx) {
    t->ufrag_index[slot].index = SFU_HASH_DELETED;
  }
}

static void table_index_addr_locked(sfu_session_table_t *t, sfu_peer_session_t *session, uint32_t idx) {
  uint32_t hash = fnv1a(&session->cold->addr, session->cold->addr_len);
  addr_match_ctx_t ctx = {t, &session->cold->addr, session->cold->addr_len};
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, true);
  if (slot != SFU_HASH_EMPTY) {
    t->addr_index[slot].hash = hash;
    t->addr_index[slot].index = idx;
  }
}

static void session_destroy_unpublished(sfu_peer_session_t *s) {
  if (!s) {
    return;
  }
  if (s->cold && s->cold->dtls.ssl) {
    sfu_dtls_conn_destroy(&s->cold->dtls);
  }
  s->active = false;
  sfu_session_free_resources(s);
  SFU_FREE(s);
}

sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!addr || addr_len == 0 || addr_len > sizeof(struct sockaddr_storage)) {
    return NULL;
  }

  pthread_rwlock_wrlock(&t->lock);

  for (uint32_t i = 0; i < t->count; i++) {
    sfu_peer_session_t *session = t->sessions[i];

    if (!session) {
      continue;
    }

    if (addr_equal(&session->cold->addr, session->cold->addr_len, addr, addr_len)) {
      atomic_fetch_add_explicit(&session->refcount, 1, memory_order_relaxed);
      pthread_rwlock_unlock(&t->lock);
      return session;
    }
  }

  uint32_t index = UINT32_MAX;
  for (uint32_t i = 0; i < t->count; i++) {
    if (!t->sessions[i]) {
      index = i;
      break;
    }
  }
  if (index == UINT32_MAX) {
    if (t->count >= t->capacity) {
      SFU_LOG_WARN("session table full (%u), rejecting new peer", t->capacity);
      pthread_rwlock_unlock(&t->lock);
      return NULL;
    }
    index = t->count++;
  }

  sfu_peer_session_t *s = SFU_CALLOC(1, sizeof(sfu_peer_session_t));
  if (!s) {
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  s->cold = SFU_CALLOC(1, sizeof(sfu_peer_session_cold_t));
  if (!s->cold) {
    SFU_FREE(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  memcpy(&s->cold->addr, addr, addr_len);
  s->cold->addr_len = addr_len;
  s->active = true;
  s->state = SFU_SESSION_NEW;
  s->fd = -1;
  atomic_store_explicit(&s->worker_owner, SFU_SESSION_OWNER_NONE, memory_order_relaxed);
  if (pthread_mutex_init(&s->answer_lock, NULL) != 0) {
    SFU_FREE(s->cold);
    SFU_FREE(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->negotiation_lock, NULL) != 0) {
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->media_lock, NULL) != 0) {
    pthread_mutex_destroy(&s->negotiation_lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->snapshot_lock, NULL) != 0) {
    pthread_mutex_destroy(&s->media_lock);
    pthread_mutex_destroy(&s->negotiation_lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->ingress_lock, NULL) != 0) {
    pthread_mutex_destroy(&s->snapshot_lock);
    pthread_mutex_destroy(&s->media_lock);
    pthread_mutex_destroy(&s->negotiation_lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  for (int i = 0; i < 128; i++) {
    s->pt_map[i] = (uint8_t)i;
  }

  s->uplink_audio.owner = s;
  s->uplink_video.owner = s;
  s->screen.owner = s;

  atomic_store_explicit(&s->receivers, NULL, memory_order_relaxed);
  atomic_store_explicit(&s->fanout_targets, NULL, memory_order_relaxed);
  atomic_store_explicit(&s->is_audience, false, memory_order_relaxed);
  atomic_store_explicit(&s->audio_send_negotiated, false, memory_order_relaxed);
  atomic_store_explicit(&s->video_send_negotiated, false, memory_order_relaxed);
  atomic_store_explicit(&s->visible, true, memory_order_relaxed);

  /* Initialize the seqlock-protected media snapshot to match the zeroed transceivers. */
  atomic_store_explicit(&s->media_snap_words[0], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media_snap_words[1], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media_snap_words[2], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media_snap_seq, 0, memory_order_relaxed);

  s->next_remote_mid = 2;
  {
    static atomic_uint_fast32_t peer_id_counter = 0;
    uint32_t id = (uint32_t)atomic_fetch_add_explicit(&peer_id_counter, 1, memory_order_relaxed) + 1;
    if (id == 0) {
      id = (uint32_t)atomic_fetch_add_explicit(&peer_id_counter, 1, memory_order_relaxed) + 1;
    }
    s->peer_id = id;
  }

  const uint32_t k_bwe_start_bps = 1500000;
  const uint32_t k_bwe_min_bps = 100000;
  const uint32_t k_bwe_max_bps = 5000000;

  s->gcc_ctx = SFU_CALLOC(1, sizeof(gcc_bwe_context_t));
  if (s->gcc_ctx) {
    gcc_bwe_init(s->gcc_ctx, k_bwe_start_bps, k_bwe_min_bps, k_bwe_max_bps);
  }

  s->twcc_history = SFU_CALLOC(1, sizeof(sfu_twcc_history_t));
  if (s->twcc_history) {
    sfu_twcc_history_init(s->twcc_history);
  }

  s->twcc_recv = SFU_CALLOC(1, sizeof(sfu_twcc_recv_tracker_t));
  if (s->twcc_recv) {
    sfu_twcc_recv_tracker_init(s->twcc_recv);
  }

  s->schedulers = SFU_CALLOC(SFU_SESSION_SCHEDULER_CAP, sizeof(sfu_session_scheduler_slot_t));
  if (!s->schedulers) {
    SFU_LOG_ERROR("failed to allocate subscriber scheduler table for new peer session");
  }
  sfu_pacer_init(&s->pacer);
  sfu_pacer_set_rate(&s->pacer, k_bwe_start_bps, (int64_t)sfu_now_us());

  s->rtx_cache = SFU_CALLOC(1, sizeof(sfu_rtx_cache_t));
  if (s->rtx_cache) {
    if (sfu_rtx_cache_init(s->rtx_cache) != 0) {
      SFU_FREE(s->rtx_cache);
      s->rtx_cache = NULL;
    }
  }
  if (!s->rtx_cache) {
    SFU_LOG_ERROR("failed to init RTX cache for new peer session");
    session_destroy_unpublished(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  if (sfu_dtls_conn_init(&s->cold->dtls, t->dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS connection for new peer session");
    session_destroy_unpublished(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  atomic_store_explicit(&s->refcount, 2, memory_order_relaxed);
  atomic_store_explicit(&s->lifecycle, SFU_SESSION_LIFECYCLE_OPEN, memory_order_relaxed);
  atomic_store_explicit(&s->accepts_work, true, memory_order_relaxed);

  t->sessions[index] = s;
  table_index_addr_locked(t, s, index);

  pthread_rwlock_unlock(&t->lock);
  return s;
}

sfu_peer_session_t *sfu_session_table_get_or_create_by_ufrag(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len, const char *ufrag,
                                                             bool allow_rebind, sfu_session_rebind_result_t *out_rebind) {
  if (out_rebind) {
    *out_rebind = SFU_SESSION_REBIND_UNCHANGED;
  }
  if (!t || !addr || addr_len == 0 || addr_len > sizeof(struct sockaddr_storage) || !ufrag || ufrag[0] == '\0') {
    return NULL;
  }

  /* Serialize the compound find/create/index operation. Existing table helpers
   * keep their own lock and remain safe for unrelated address lookups. */
  pthread_mutex_lock(&t->ice_lock);

  sfu_peer_session_t *session = sfu_session_table_find_by_ufrag(t, ufrag);
  if (session) {
    bool addr_changed = !addr_equal(&session->cold->addr, session->cold->addr_len, addr, addr_len);
    if (addr_changed && allow_rebind) {
      pthread_mutex_lock(&session->answer_lock);
      bool rebound = sfu_session_table_rebind_addr(t, session, addr, addr_len);
      pthread_mutex_unlock(&session->answer_lock);
      if (out_rebind) {
        *out_rebind = rebound ? SFU_SESSION_REBIND_APPLIED : SFU_SESSION_REBIND_REJECTED;
      }
      if (!rebound) {
        sfu_session_release(session);
        pthread_mutex_unlock(&t->ice_lock);
        return NULL;
      }
    }
    pthread_mutex_unlock(&t->ice_lock);
    return session;
  }

  session = sfu_session_table_get_or_create(t, addr, addr_len);
  if (!session) {
    pthread_mutex_unlock(&t->ice_lock);
    return NULL;
  }

  if (session->cold->ufrag[0] != '\0' && strcmp(session->cold->ufrag, ufrag) != 0) {
    SFU_LOG_WARN("session address already belongs to a different ufrag (%s != %s)", session->cold->ufrag, ufrag);
    sfu_session_release(session);
    pthread_mutex_unlock(&t->ice_lock);
    return NULL;
  }

  if (session->cold->ufrag[0] == '\0') {
    snprintf(session->cold->ufrag, sizeof(session->cold->ufrag), "%s", ufrag);
  }
  if (!sfu_session_table_index_ufrag(t, session)) {
    sfu_peer_session_t *winner = sfu_session_table_find_by_ufrag(t, ufrag);
    sfu_session_release(session);
    session = winner;
  }

  pthread_mutex_unlock(&t->ice_lock);
  return session;
}

sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!addr || addr_len == 0) {
    return NULL;
  }
  uint32_t hash = fnv1a(addr, addr_len);
  addr_match_ctx_t ctx = {t, addr, addr_len};
  pthread_rwlock_rdlock(&t->lock);
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, false);
  sfu_peer_session_t *result = NULL;
  if (slot != SFU_HASH_EMPTY && t->addr_index[slot].index != SFU_HASH_EMPTY && t->addr_index[slot].index < t->count) {
    result = t->sessions[t->addr_index[slot].index];
    if (result) {
      atomic_fetch_add_explicit(&result->refcount, 1, memory_order_relaxed);
    }
  }
  pthread_rwlock_unlock(&t->lock);
  return result;
}

sfu_peer_session_t *sfu_session_table_find_by_ufrag(sfu_session_table_t *t, const char *ufrag) {
  if (!ufrag || ufrag[0] == '\0') {
    return NULL;
  }

  uint32_t hash = fnv1a(ufrag, strlen(ufrag));
  ufrag_match_ctx_t ctx = {t, ufrag};

  pthread_rwlock_rdlock(&t->lock);
  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches_direct, &ctx, false);
  sfu_peer_session_t *result = NULL;
  if (slot != SFU_HASH_EMPTY && t->ufrag_index[slot].index != SFU_HASH_EMPTY && t->ufrag_index[slot].index < t->count) {
    result = t->sessions[t->ufrag_index[slot].index];
    if (result) {
      atomic_fetch_add_explicit(&result->refcount, 1, memory_order_relaxed);
    }
  }
  pthread_rwlock_unlock(&t->lock);
  return result;
}

uint32_t sfu_session_table_foreach(sfu_session_table_t *t, sfu_session_iter_fn fn, void *user) {
  if (!t || !fn) {
    return 0;
  }

  pthread_rwlock_rdlock(&t->lock);
  uint32_t pin_capacity = t->count ? t->count : 1;
  sfu_peer_session_t **pinned = SFU_CALLOC(pin_capacity, sizeof(*pinned));
  if (!pinned) {
    pthread_rwlock_unlock(&t->lock);
    return 0;
  }
  uint32_t pinned_count = 0;

  for (uint32_t i = 0; i < t->count; i++) {
    sfu_peer_session_t *s = t->sessions[i];
    if (!s) {
      continue;
    }
    if (atomic_load_explicit(&s->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN) {
      continue;
    }
    atomic_fetch_add_explicit(&s->refcount, 1, memory_order_relaxed);
    pinned[pinned_count++] = s;
  }
  pthread_rwlock_unlock(&t->lock);

  for (uint32_t i = 0; i < pinned_count; i++) {
    fn(pinned[i], user);
    sfu_session_release(pinned[i]);
  }

  SFU_FREE(pinned);
  return pinned_count;
}

bool sfu_session_begin_close(sfu_session_table_t *t, sfu_peer_session_t *s) {
  if (!t || !s) {
    return false;
  }

  pthread_rwlock_wrlock(&t->lock);

  if (atomic_load_explicit(&s->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN) {
    pthread_rwlock_unlock(&t->lock);
    return false;
  }

  atomic_store_explicit(&s->lifecycle, SFU_SESSION_LIFECYCLE_CLOSING, memory_order_release);
  atomic_store_explicit(&s->accepts_work, false, memory_order_release);

  uint32_t idx = table_member_index(t, s);
  if (idx != UINT32_MAX) {
    table_remove_addr_hash(t, s, idx);
    table_remove_ufrag_hash(t, s, idx);
    t->sessions[idx] = NULL;
  }

  pthread_rwlock_unlock(&t->lock);

  sfu_room_t *room = (sfu_room_t *)s->room;
  if (room) {
    room_remove_peer(room, s);
  }

  sfu_session_release(s);
  return true;
}

void sfu_session_table_remove(sfu_session_table_t *t, sfu_peer_session_t *s) { (void)sfu_session_begin_close(t, s); }

bool sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!t || !s || !addr || addr_len == 0 || addr_len > sizeof(s->cold->addr)) {
    return false;
  }

  pthread_rwlock_wrlock(&t->lock);

  uint32_t idx = table_member_index(t, s);
  if (idx == UINT32_MAX || atomic_load_explicit(&s->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN ||
      !atomic_load_explicit(&s->accepts_work, memory_order_acquire)) {
    pthread_rwlock_unlock(&t->lock);
    return false;
  }

  uint32_t new_hash = fnv1a(addr, addr_len);
  addr_match_ctx_t new_ctx = {t, addr, addr_len};
  uint32_t occupied = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, new_hash, addr_matches_direct, &new_ctx, false);
  if (occupied != SFU_HASH_EMPTY && t->addr_index[occupied].index != idx) {
    pthread_rwlock_unlock(&t->lock);
    SFU_LOG_WARN("rejecting address rebind: target tuple belongs to another session");
    return false;
  }

  table_remove_addr_hash(t, s, idx);
  memcpy(&s->cold->addr, addr, addr_len);
  s->cold->addr_len = addr_len;
  table_index_addr_locked(t, s, idx);

  pthread_rwlock_unlock(&t->lock);
  return true;
}

bool sfu_session_table_index_ufrag(sfu_session_table_t *t, sfu_peer_session_t *session) {
  if (!t || !session || session->cold->ufrag[0] == '\0') {
    return false;
  }

  pthread_rwlock_wrlock(&t->lock);

  uint32_t idx = table_member_index(t, session);
  if (idx == UINT32_MAX || atomic_load_explicit(&session->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN ||
      !atomic_load_explicit(&session->accepts_work, memory_order_acquire)) {
    pthread_rwlock_unlock(&t->lock);
    return false;
  }

  uint32_t hash = fnv1a(session->cold->ufrag, strlen(session->cold->ufrag));
  ufrag_match_ctx_t ctx = {t, session->cold->ufrag};
  uint32_t existing = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches_direct, &ctx, false);
  if (existing != SFU_HASH_EMPTY && t->ufrag_index[existing].index != idx) {
    pthread_rwlock_unlock(&t->lock);
    SFU_LOG_WARN("rejecting duplicate session for ufrag=%s", session->cold->ufrag);
    return false;
  }

  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches_direct, &ctx, true);
  if (slot == SFU_HASH_EMPTY) {
    pthread_rwlock_unlock(&t->lock);
    return false;
  }
  t->ufrag_index[slot].hash = hash;
  t->ufrag_index[slot].index = idx;

  pthread_rwlock_unlock(&t->lock);
  return true;
}

void sfu_session_table_destroy(sfu_session_table_t *t) {
  sfu_peer_session_t **orphans = SFU_CALLOC(t->count ? t->count : 1, sizeof(*orphans));
  uint32_t orphan_count = 0;

  pthread_rwlock_wrlock(&t->lock);

  for (uint32_t i = 0; i < t->count; i++) {
    sfu_peer_session_t *s = t->sessions[i];
    if (!s) {
      continue;
    }
    if (atomic_load_explicit(&s->lifecycle, memory_order_acquire) == SFU_SESSION_LIFECYCLE_OPEN) {
      atomic_store_explicit(&s->lifecycle, SFU_SESSION_LIFECYCLE_CLOSING, memory_order_release);
      atomic_store_explicit(&s->accepts_work, false, memory_order_release);
      table_remove_addr_hash(t, s, i);
      table_remove_ufrag_hash(t, s, i);
    }
    t->sessions[i] = NULL;
    if (orphans) {
      orphans[orphan_count++] = s;
    }
  }

  pthread_rwlock_unlock(&t->lock);

  for (uint32_t i = 0; i < orphan_count; i++) {
    sfu_room_t *room = (sfu_room_t *)orphans[i]->room;
    if (room) {
      room_remove_peer(room, orphans[i]);
    }
    sfu_session_release(orphans[i]);
  }
  SFU_FREE(orphans);

  SFU_FREE(t->sessions);
  t->sessions = NULL;
  t->count = 0;
  t->capacity = 0;

  pthread_mutex_destroy(&t->ice_lock);
  pthread_rwlock_destroy(&t->lock);
}

bool sfu_session_apply_pending_answer(sfu_peer_session_t *session, const sfu_pending_answer_t *answer, int fd, bool *role_changed, bool *media_changed) {
  if (!session || !answer || !answer->valid) {
    return false;
  }

  pthread_mutex_lock(&session->answer_lock);
  uint32_t applied = atomic_load_explicit(&session->applied_answer_generation, memory_order_acquire);
  if (answer->generation <= applied) {
    pthread_mutex_unlock(&session->answer_lock);
    return false;
  }

  bool old_audience = atomic_load_explicit(&session->is_audience, memory_order_acquire);
  bool role_diff = old_audience != answer->is_audience;
  bool changed = false;

  if (session->room && role_diff) {
    role_diff = room_update_peer_role((sfu_room_t *)session->room, session, answer->is_audience);
  } else if (!session->room) {
    atomic_store_explicit(&session->is_audience, answer->is_audience, memory_order_release);
  }

  if (answer->audio_section_present) {
    atomic_store_explicit(&session->audio_send_negotiated, answer->audio_sends && !answer->is_audience, memory_order_release);
  }
  if (answer->video_section_present) {
    atomic_store_explicit(&session->video_send_negotiated, answer->video_sends && !answer->is_audience, memory_order_release);
  }

  pthread_mutex_lock(&session->media_lock);
  uint32_t audio_ssrc = answer->audio_section_present ? (answer->audio_sends ? answer->audio_ssrc : 0) : session->uplink_audio.ssrc;
  uint32_t video_ssrc = answer->video_section_present ? (answer->video_sends ? answer->video_ssrc : 0) : session->uplink_video.ssrc;
  uint32_t rtx_ssrc = answer->video_section_present ? (answer->video_sends ? answer->rtx_ssrc : 0) : session->uplink_video.rtx_ssrc;
  uint8_t video_pt = answer->video_pt != 0 ? answer->video_pt : session->uplink_video.payload_type;
  uint8_t rtx_pt = answer->rtx_pt != 0 ? answer->rtx_pt : session->uplink_video.rtx_payload_type;
  sfu_video_codec_t codec = answer->video_codec != SFU_VIDEO_CODEC_NONE ? (sfu_video_codec_t)answer->video_codec : session->uplink_video.codec;
  bool current_audience = atomic_load_explicit(&session->is_audience, memory_order_acquire);
  bool audio_active = !current_audience && audio_ssrc != 0;
  bool video_active = !current_audience && video_ssrc != 0;

  changed = session->uplink_audio.ssrc != audio_ssrc || session->uplink_audio.active != audio_active || session->uplink_video.ssrc != video_ssrc ||
            session->uplink_video.rtx_ssrc != rtx_ssrc || session->uplink_video.active != video_active || session->uplink_video.payload_type != video_pt ||
            session->uplink_video.rtx_payload_type != rtx_pt || session->uplink_video.codec != codec;

  session->uplink_audio.ssrc = audio_ssrc;
  session->uplink_audio.active = audio_active;
  session->uplink_video.ssrc = video_ssrc;
  session->uplink_video.rtx_ssrc = rtx_ssrc;
  session->uplink_video.active = video_active;
  session->uplink_video.payload_type = video_pt;
  session->uplink_video.rtx_payload_type = rtx_pt;
  session->uplink_video.codec = codec;
  session->twcc_recv_extmap_id = answer->twcc_recv_extmap_id;
  session->twcc_send_extmap_id = answer->twcc_send_extmap_id;
  sfu_session_publish_media(session);
  pthread_mutex_unlock(&session->media_lock);
  if (answer->peer_id != 0) {
    session->peer_id = answer->peer_id;
  }
  session->user_id = answer->user_id;
  session->fd = fd;
  for (int i = 0; i < 128; i++) {
    session->pt_map[i] = (uint8_t)i;
  }
  atomic_store_explicit(&session->applied_answer_generation, answer->generation, memory_order_release);
  pthread_mutex_unlock(&session->answer_lock);

  if (role_changed) {
    *role_changed = role_diff;
  }
  if (media_changed) {
    *media_changed = changed;
  }
  return true;
}

void sfu_session_request_keyframe(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir) {
  if (!w || !publisher) {
    SFU_LOG_WARN("[KF-DBG] sfu_session_request_keyframe called with NULL worker or publisher");
    return;
  }

  uint16_t owner_worker = sfu_session_owner_worker(publisher);
  if (owner_worker != w->worker_index) {
    SFU_LOG_DEBUG("[KF-DBG] Offloading KF request: current worker %u -> publisher worker %u (pub peer_id=%u)", w->worker_index, owner_worker,
                  publisher->peer_id);
    if (w->mesh) {
      bool queued = sfu_fanout_mesh_enqueue_keyframe_request(w->mesh, w->worker_index, owner_worker, publisher);
      if (!queued) {
        SFU_LOG_ERROR("[KF-DBG] FAILED to enqueue cross-worker KF request from %u to %u", w->worker_index, owner_worker);
      }
    } else {
      SFU_LOG_ERROR("[KF-DBG] Worker %u mesh is NULL! Cannot dispatch cross-worker KF request", w->worker_index);
    }
    return;
  }

  pthread_mutex_lock(&publisher->media_lock);
  uint32_t media_ssrc = publisher->uplink_video.ssrc;
  pthread_mutex_unlock(&publisher->media_lock);

  SFU_LOG_DEBUG("[KF-DBG] Executing KF request on owner worker %u for pub peer_id=%u (uplink SSRC=%u)", w->worker_index, publisher->peer_id, media_ssrc);

  int64_t now = (int64_t)sfu_now_ms();
  if (publisher->last_pli_time != 0 && now - publisher->last_pli_time < SFU_SESSION_KF_THROTTLE_MS) {
    SFU_LOG_DEBUG("worker %u: KF request for publisher %u coalesced (last PLI %" PRId64 " ms ago)", w->worker_index, publisher->peer_id,
                  now - publisher->last_pli_time);
    return;
  }

  publisher->last_pli_time = now;

  if (media_ssrc == 0) {
    SFU_LOG_WARN("[KF-DBG] Cannot send PLI/FIR: Publisher %u video SSRC is 0", publisher->peer_id);
    return;
  }

  sfu_packet_t *rtcp_pkt = sfu_packet_pool_alloc(w->pp);
  if (!rtcp_pkt) {
    return;
  }

  SFU_LOG_INFO("Worker %u executing PLI output to publisher peer %u SSRC %u", w->worker_index, publisher->peer_id, media_ssrc);

  int rtcp_len = 0;
  uint32_t sfu_sender_ssrc = 1;


  if (use_fir) {
    rtcp_len = sfu_rtcp_build_fir(sfu_sender_ssrc, media_ssrc, &publisher->fir_seq, rtcp_pkt->data, rtcp_pkt->cap);
  } else {
    rtcp_len = sfu_rtcp_build_pli(sfu_sender_ssrc, media_ssrc, rtcp_pkt->data, rtcp_pkt->cap);
  }

  if (rtcp_len > 0) {
    if (sfu_srtp_protect_rtcp(&publisher->srtp, rtcp_pkt->data, &rtcp_len, rtcp_pkt->cap)) {
      rtcp_pkt->len = (uint32_t)rtcp_len;

      int sent = sfu_ring_queue_send_zc(&w->send_ring, rtcp_pkt, (const struct sockaddr *)&publisher->cold->addr, publisher->cold->addr_len);
      if (sent != 0) {
        SFU_LOG_ERROR("Failed to enqueue PLI to send_ring for peer %u", publisher->peer_id);
      }
    } else {
      SFU_LOG_WARN("Failed to SRTP protect keyframe request for peer %u", publisher->peer_id);
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtcp_pkt);
}

void sfu_session_maybe_send_twcc_feedback(sfu_worker_t *w, sfu_peer_session_t *publisher) {
  if (!w || !publisher || !publisher->twcc_recv) {
    return;
  }
  if (sfu_session_owner_worker(publisher) != w->worker_index) {
    return;
  }

  sfu_twcc_recv_tracker_t *t = publisher->twcc_recv;
  if (!sfu_twcc_recv_tracker_pending(t)) {
    return;
  }

  int64_t now_us = (int64_t)sfu_now_us();
  if (t->last_feedback_us != 0 && now_us - t->last_feedback_us < SFU_TWCC_FEEDBACK_INTERVAL_US) {
    return;
  }

  uint32_t media_ssrc = publisher->uplink_video.ssrc ? publisher->uplink_video.ssrc : publisher->uplink_audio.ssrc;
  uint32_t sfu_sender_ssrc = 1;

  for (int burst = 0; burst < 8; burst++) {
    if (!sfu_twcc_recv_tracker_pending(t)) {
      break;
    }

    sfu_packet_t *rtcp_pkt = sfu_packet_pool_alloc(w->pp);
    if (!rtcp_pkt) {
      break;
    }

    int rtcp_len = sfu_twcc_feedback_build(t, sfu_sender_ssrc, media_ssrc, now_us, rtcp_pkt->data, rtcp_pkt->cap);
    if (rtcp_len <= 0) {
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtcp_pkt);
      if (rtcp_len < 0) {
        break;
      }
      continue;
    }

    if (sfu_srtp_protect_rtcp(&publisher->srtp, rtcp_pkt->data, &rtcp_len, rtcp_pkt->cap)) {
      rtcp_pkt->len = (uint32_t)rtcp_len;
      int sent = sfu_ring_queue_send_zc(&w->send_ring, rtcp_pkt, (const struct sockaddr *)&publisher->cold->addr, publisher->cold->addr_len);
      if (sent != 0) {
        SFU_LOG_ERROR("Failed to enqueue TWCC feedback to send_ring for peer %u", publisher->peer_id);
      }
    } else {
      SFU_LOG_WARN("Failed to SRTP protect TWCC feedback for peer %u", publisher->peer_id);
    }

    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtcp_pkt);
  }
}
