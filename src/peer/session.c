#include "peer/session.h"
#include <assert.h>
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

/* ---------------------------------------------------------------------------
 * Receiver snapshot helpers (F-03/F-04)
 * ------------------------------------------------------------------------- */

/* Lock-free acquire-with-retain: the pointer is only replaced (release
 * store) while the outgoing snapshot still holds the writer's reference, so
 * the CAS below can never resurrect a fully-dead snapshot. Memory-reuse ABA
 * is impossible while the CAS could succeed, because the outgoing snapshot's
 * writer reference is only dropped after the replacement store.
 *
 * Concurrency note: publishing (room_media_graph.c) is serialized by the
 * room lock, so there is at most one writer per session. The single-writer
 * invariant matters here: a CAS retry after a failed refcount increment
 * reloads the session pointer, and with one writer that reload sees either
 * the same live snapshot or its fully-published replacement — never a
 * half-retired one. */
sfu_receiver_snapshot_t *sfu_session_receivers_acquire(const sfu_peer_session_t *s) {
  sfu_receiver_snapshot_t *snap = atomic_load_explicit(&s->receivers, memory_order_acquire);
  while (snap) {
    uint32_t rc = atomic_load_explicit(&snap->refcount, memory_order_relaxed);
    if (rc == 0) {
      /* Being torn down by its last releaser; retry for the replacement. */
      snap = atomic_load_explicit(&s->receivers, memory_order_acquire);
      continue;
    }
    if (atomic_compare_exchange_weak_explicit(&snap->refcount, &rc, rc + 1, memory_order_acquire, memory_order_relaxed)) {
      return snap;
    }
  }
  return NULL;
}

void sfu_receiver_snapshot_release(sfu_receiver_snapshot_t *snap) {
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

/* Publishes a fully-built snapshot on the owning session and retires the old
 * one by dropping the writer's reference. Call with the room lock held. */
void sfu_session_publish_receivers(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) {
  sfu_receiver_snapshot_t *old = atomic_load_explicit(&owner->receivers, memory_order_acquire);
  atomic_store_explicit(&owner->receivers, new_snap, memory_order_release);
  sfu_receiver_snapshot_release(old);
}

/* ---------------------------------------------------------------------------
 * Session teardown
 * ------------------------------------------------------------------------- */

/* Centralized teardown. Runs exactly once, when the last refcount is dropped.
 * `active` is never cleared before this point (logical close keeps it set),
 * so initialized DTLS/SRTP state is always destroyed here. */
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

  sfu_receiver_snapshot_t *snap = atomic_load_explicit(&s->receivers, memory_order_acquire);
  if (snap) {
    atomic_store_explicit(&s->receivers, NULL, memory_order_release);
    sfu_receiver_snapshot_release(snap);
  }

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
  if (s->scheduler) {
    SFU_FREE(s->scheduler);
    s->scheduler = NULL;
  }
  if (s->cold) {
    SFU_FREE(s->cold);
    s->cold = NULL;
  }
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

/* Table-lock-held helpers. */

/* Returns the session's member index, or UINT32_MAX if not a live member. */
static uint32_t table_member_index(const sfu_session_table_t *t, const sfu_peer_session_t *session) {
  for (uint32_t i = 0; i < t->count; i++) {
    if (t->sessions[i] == session) {
      return i;
    }
  }
  return UINT32_MAX;
}

/* A hash entry is only matched while the member index it references is still
 * live; the addr/ufrag bytes are compared against the session's cold data. */
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

/* Removes the addr hash entry referencing member index `idx`, if any. */
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

/* Removes the ufrag hash entry referencing member index `idx`, if any. */
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

/* Indexes the session's current address. Call with the table lock held; the
 * caller must have verified membership. */
static void table_index_addr_locked(sfu_session_table_t *t, sfu_peer_session_t *session, uint32_t idx) {
  uint32_t hash = fnv1a(&session->cold->addr, session->cold->addr_len);
  addr_match_ctx_t ctx = {t, &session->cold->addr, session->cold->addr_len};
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, true);
  if (slot != SFU_HASH_EMPTY) {
    t->addr_index[slot].hash = hash;
    t->addr_index[slot].index = idx;
  }
}

/* ---------------------------------------------------------------------------
 * Creation
 * ------------------------------------------------------------------------- */

/* Frees a session that failed part-way through construction, before it was
 * ever published. Safe on partially-initialized sessions: DTLS is destroyed
 * only when its SSL object exists. */
static void session_destroy_unpublished(sfu_peer_session_t *s) {
  if (!s) {
    return;
  }
  if (s->cold && s->cold->dtls.ssl) {
    sfu_dtls_conn_destroy(&s->cold->dtls);
  }
  s->active = false; /* suppress the established-state teardown paths */
  sfu_session_free_resources(s);
  SFU_FREE(s);
}

sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!addr || addr_len == 0 || addr_len > sizeof(struct sockaddr_storage)) {
    return NULL;
  }

  pthread_mutex_lock(&t->lock);

  for (uint32_t i = 0; i < t->count; i++) {
    sfu_peer_session_t *session = t->sessions[i];

    if (!session) {
      continue;
    }

    if (addr_equal(&session->cold->addr, session->cold->addr_len, addr, addr_len)) {
      /* Caller pin: the table slot keeps the session alive under the lock,
       * so this increment is safe. */
      atomic_fetch_add_explicit(&session->refcount, 1, memory_order_relaxed);
      pthread_mutex_unlock(&t->lock);
      return session;
    }
  }

  /* First NULL hole wins; extend count only when there is no hole. */
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
      pthread_mutex_unlock(&t->lock);
      return NULL;
    }
    index = t->count++;
  }

  sfu_peer_session_t *s = SFU_CALLOC(1, sizeof(sfu_peer_session_t));
  if (!s) {
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  s->cold = SFU_CALLOC(1, sizeof(sfu_peer_session_cold_t));
  if (!s->cold) {
    SFU_FREE(s);
    if (index + 1 == t->count) {
      t->count--;
    }
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

  atomic_store_explicit(&s->receivers, NULL, memory_order_relaxed);

  s->next_remote_mid = 2;

  /* All fallible initialization happens BEFORE the session is published into
   * the table or any hash, so a construction failure leaves no observable
   * trace (no slot, no hash entry, no way for another thread to find it). */

  s->gcc_ctx = SFU_CALLOC(1, sizeof(gcc_bwe_context_t));
  if (s->gcc_ctx) {
    gcc_bwe_init(s->gcc_ctx, 300000, 50000, 5000000);
  }

  s->twcc_history = SFU_CALLOC(1, sizeof(sfu_twcc_history_t));
  if (s->twcc_history) {
    sfu_twcc_history_init(s->twcc_history);
  }

  s->scheduler = SFU_CALLOC(1, sizeof(sfu_subscriber_scheduler_t));
  if (s->scheduler) {
    sfu_subscriber_scheduler_init(s->scheduler, 0);
  }

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
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  if (sfu_dtls_conn_init(&s->cold->dtls, t->dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS connection for new peer session");
    session_destroy_unpublished(s);
    if (index + 1 == t->count) {
      t->count--;
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
  }

  /* Publish: table ref = 1, caller pin = 1, OPEN and accepting work. */
  atomic_store_explicit(&s->refcount, 2, memory_order_relaxed);
  atomic_store_explicit(&s->lifecycle, SFU_SESSION_LIFECYCLE_OPEN, memory_order_relaxed);
  atomic_store_explicit(&s->accepts_work, true, memory_order_relaxed);

  t->sessions[index] = s;
  table_index_addr_locked(t, s, index);

  pthread_mutex_unlock(&t->lock);
  return s;
}

/* ---------------------------------------------------------------------------
 * Lookups (return a caller pin)
 * ------------------------------------------------------------------------- */

sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!addr || addr_len == 0) {
    return NULL;
  }
  uint32_t hash = fnv1a(addr, addr_len);
  addr_match_ctx_t ctx = {t, addr, addr_len};
  pthread_mutex_lock(&t->lock);
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, false);
  sfu_peer_session_t *result = NULL;
  if (slot != SFU_HASH_EMPTY && t->addr_index[slot].index != SFU_HASH_EMPTY && t->addr_index[slot].index < t->count) {
    result = t->sessions[t->addr_index[slot].index];
    if (result) {
      atomic_fetch_add_explicit(&result->refcount, 1, memory_order_relaxed);
    }
  }
  pthread_mutex_unlock(&t->lock);
  return result;
}

sfu_peer_session_t *sfu_session_table_find_by_ufrag(sfu_session_table_t *t, const char *ufrag) {
  if (!ufrag || ufrag[0] == '\0') {
    return NULL;
  }

  uint32_t hash = fnv1a(ufrag, strlen(ufrag));
  ufrag_match_ctx_t ctx = {t, ufrag};

  pthread_mutex_lock(&t->lock);
  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches_direct, &ctx, false);
  sfu_peer_session_t *result = NULL;
  if (slot != SFU_HASH_EMPTY && t->ufrag_index[slot].index != SFU_HASH_EMPTY && t->ufrag_index[slot].index < t->count) {
    result = t->sessions[t->ufrag_index[slot].index];
    if (result) {
      atomic_fetch_add_explicit(&result->refcount, 1, memory_order_relaxed);
    }
  }
  pthread_mutex_unlock(&t->lock);
  return result;
}

/* ---------------------------------------------------------------------------
 * Close
 * ------------------------------------------------------------------------- */

bool sfu_session_begin_close(sfu_session_table_t *t, sfu_peer_session_t *s) {
  if (!t || !s) {
    return false;
  }

  pthread_mutex_lock(&t->lock);

  if (atomic_load_explicit(&s->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN) {
    /* Already closing/closed: idempotent no-op. The slot and hash entries
     * were removed by the first close; nothing to do. */
    pthread_mutex_unlock(&t->lock);
    return false;
  }

  /* First OPEN -> CLOSING transition. */
  atomic_store_explicit(&s->lifecycle, SFU_SESSION_LIFECYCLE_CLOSING, memory_order_release);
  atomic_store_explicit(&s->accepts_work, false, memory_order_release);

  /* Remove ALL hash entries referencing this session's member index (scan by
   * index, not by any active-state predicate) and clear the table slot. The
   * slot becomes a reusable hole for the next create. `active` deliberately
   * stays true so final teardown destroys initialized DTLS/SRTP state. */
  uint32_t idx = table_member_index(t, s);
  if (idx != UINT32_MAX) {
    table_remove_addr_hash(t, s, idx);
    table_remove_ufrag_hash(t, s, idx);
    t->sessions[idx] = NULL;
  }

  pthread_mutex_unlock(&t->lock);

  /* Detach from the room exactly once, outside the table lock. Lock ordering:
   * the close path never holds the table lock while taking the room lock, and
   * room_media_graph never takes the table lock. */
  sfu_room_t *room = (sfu_room_t *)s->room;
  if (room) {
    room_remove_peer(room, s);
  }

  /* Drop the table's reference; remaining caller/snapshot pins keep the
   * allocation alive. */
  sfu_session_release(s);
  return true;
}

void sfu_session_table_remove(sfu_session_table_t *t, sfu_peer_session_t *s) { (void)sfu_session_begin_close(t, s); }

/* ---------------------------------------------------------------------------
 * Rebind / ufrag indexing (rejected once closing or not a member)
 * ------------------------------------------------------------------------- */

bool sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len) {
  if (!t || !s || !addr || addr_len == 0 || addr_len > sizeof(s->cold->addr)) {
    return false;
  }

  pthread_mutex_lock(&t->lock);

  uint32_t idx = table_member_index(t, s);
  if (idx == UINT32_MAX || atomic_load_explicit(&s->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN ||
      !atomic_load_explicit(&s->accepts_work, memory_order_acquire)) {
    pthread_mutex_unlock(&t->lock);
    return false;
  }

  table_remove_addr_hash(t, s, idx);
  memcpy(&s->cold->addr, addr, addr_len);
  s->cold->addr_len = addr_len;
  table_index_addr_locked(t, s, idx);

  pthread_mutex_unlock(&t->lock);
  return true;
}

bool sfu_session_table_index_ufrag(sfu_session_table_t *t, sfu_peer_session_t *session) {
  if (!t || !session || session->cold->ufrag[0] == '\0') {
    return false;
  }

  pthread_mutex_lock(&t->lock);

  uint32_t idx = table_member_index(t, session);
  if (idx == UINT32_MAX || atomic_load_explicit(&session->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN ||
      !atomic_load_explicit(&session->accepts_work, memory_order_acquire)) {
    pthread_mutex_unlock(&t->lock);
    return false;
  }

  uint32_t hash = fnv1a(session->cold->ufrag, strlen(session->cold->ufrag));
  ufrag_match_ctx_t ctx = {t, session->cold->ufrag};
  uint32_t slot = addr_probe(t->ufrag_index, SFU_SESSION_UFRAG_HASH_SLOTS, hash, ufrag_matches_direct, &ctx, true);
  if (slot != SFU_HASH_EMPTY) {
    t->ufrag_index[slot].hash = hash;
    t->ufrag_index[slot].index = idx;
  }

  pthread_mutex_unlock(&t->lock);
  return true;
}

/* ---------------------------------------------------------------------------
 * Table destroy
 * ------------------------------------------------------------------------- */

void sfu_session_table_destroy(sfu_session_table_t *t) {
  /* No concurrent table users may exist here (workers joined, signaling
   * stopped, all caller pins released); the lock below is defensive. */
  sfu_peer_session_t **orphans = SFU_CALLOC(t->count ? t->count : 1, sizeof(*orphans));
  uint32_t orphan_count = 0;

  pthread_mutex_lock(&t->lock);

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

  pthread_mutex_unlock(&t->lock);

  /* Detach rooms and drop table refs outside the lock. */
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

  pthread_mutex_destroy(&t->lock);
}

/* ---------------------------------------------------------------------------
 * Keyframe requests
 * ------------------------------------------------------------------------- */

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
