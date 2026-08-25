#include "peer/session.h"
#include <assert.h>
#include <inttypes.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "congestion/gcc.h"
#include "congestion/pacer.h"
#include "congestion/twcc_feedback.h"
#include "congestion/twcc_history.h"
#include "media/svc/layer_scheduler.h"
#include "protocol/signaling/signaling.h"
#include "room/room_media_graph.h"
#include "rtcp/rtcp_kf.h"
#include "rtp/rtx.h"
#include "runtime/epoch_reclaimer.h"
#include "runtime/routing_context.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/metrics.h"

#define SFU_SESSION_KF_THROTTLE_MS 1000
#define SFU_SNAPSHOT_HAZARD_SLOTS 256
#define SFU_BWE_START_BPS 1500000u
#define SFU_BWE_MIN_BPS 100000u
#define SFU_BWE_MAX_BPS 5000000u
#define SFU_REMB_CONTRIBUTION_MAX_AGE_US 2000000ULL
#define SFU_REMB_NORMAL_INTERVAL_US 500000LL
#define SFU_REMB_DECREASE_INTERVAL_US 100000LL
#define SFU_REMB_REFRESH_INTERVAL_US 2000000LL
#ifdef SFU_DIAG_LOG
#define SFU_CONGESTION_DIAG_INTERVAL_US 2000000ULL
#define SFU_CONGESTION_DIAG_LINE_CAP 2048u
#endif

typedef struct {
  sfu_session_table_t *t;
  const struct sockaddr_storage *addr;
  socklen_t addr_len;
} addr_match_ctx_t;

typedef struct {
  sfu_session_table_t *t;
  const char *ufrag;
} ufrag_match_ctx_t;

typedef struct {
  _Atomic uint32_t refcount;
} sfu_snapshot_ref_t;

static _Atomic(void *) snapshot_hazards[SFU_SNAPSHOT_HAZARD_SLOTS];
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

static _Atomic(void *) *snapshot_hazard_for_thread(void) {
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

static void *snapshot_acquire(const sfu_peer_session_t *owner, const _Atomic(void *) *source) {
  _Atomic(void *) *hazard = snapshot_hazard_for_thread();
  if (!hazard) {
    pthread_mutex_lock((pthread_mutex_t *)&owner->graph.lock);
    void *snap = atomic_load_explicit(source, memory_order_acquire);
    if (snap) {
      atomic_fetch_add_explicit(&((sfu_snapshot_ref_t *)snap)->refcount, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock((pthread_mutex_t *)&owner->graph.lock);
    return snap;
  }

  for (;;) {
    void *snap = atomic_load_explicit(source, memory_order_seq_cst);
    atomic_store_explicit(hazard, snap, memory_order_seq_cst);
    if (snap != atomic_load_explicit(source, memory_order_seq_cst)) {
      atomic_store_explicit(hazard, NULL, memory_order_seq_cst);
      continue;
    }
    if (snap) {
      atomic_fetch_add_explicit(&((sfu_snapshot_ref_t *)snap)->refcount, 1, memory_order_relaxed);
    }
    atomic_store_explicit(hazard, NULL, memory_order_seq_cst);
    return snap;
  }
}

static void snapshot_wait_unhazarded(void *snap) {
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

bool sfu_session_recompute_video_activity_locked(sfu_peer_session_t *session) {
  bool is_audience = atomic_load_explicit(&session->is_audience, memory_order_acquire);
  bool camera_active = !is_audience && atomic_load_explicit(&session->media.camera_enabled, memory_order_acquire) &&
                       atomic_load_explicit(&session->media.video_send_negotiated, memory_order_acquire) &&
                       atomic_load_explicit(&session->media.camera_rtp_observed, memory_order_acquire) && session->media.uplink_video.ssrc != 0;
  bool screen_active = !is_audience && atomic_load_explicit(&session->media.screen_enabled, memory_order_acquire) &&
                       atomic_load_explicit(&session->media.screen_send_negotiated, memory_order_acquire) &&
                       atomic_load_explicit(&session->media.screen_rtp_observed, memory_order_acquire) && session->media.screen.ssrc != 0;
  bool changed = session->media.uplink_video.active != camera_active || session->media.screen.active != screen_active;
  session->media.uplink_video.active = camera_active;
  session->media.screen.active = screen_active;
  return changed;
}

bool sfu_session_ensure_video_runtime(sfu_peer_session_t *session) {
  if (!session) {
    return false;
  }
  if (sfu_session_video_runtime_ready(session)) {
    return true;
  }

  pthread_mutex_lock(&session->media.lock);
  if (sfu_session_video_runtime_ready(session)) {
    pthread_mutex_unlock(&session->media.lock);
    return true;
  }
  atomic_store_explicit(&session->egress.video_runtime_state, SFU_VIDEO_RUNTIME_INITIALIZING, memory_order_release);

  gcc_bwe_context_t *gcc = SFU_CALLOC(1, sizeof(*gcc));
  sfu_twcc_history_t *history = SFU_CALLOC(1, sizeof(*history));
  sfu_twcc_recv_tracker_t *recv = SFU_CALLOC(1, sizeof(*recv));
  sfu_layer_scheduler_slot_t *schedulers = SFU_CALLOC(SFU_LAYER_SCHEDULER_CAP, sizeof(*schedulers));
  sfu_rtx_cache_t *rtx = SFU_CALLOC(1, sizeof(*rtx));
  bool ok = gcc && history && recv && schedulers && rtx && sfu_rtx_cache_init(rtx) == 0;
  if (!ok) {
    if (rtx) {
      SFU_FREE(rtx);
    }
    SFU_FREE(schedulers);
    SFU_FREE(recv);
    SFU_FREE(history);
    SFU_FREE(gcc);
    atomic_store_explicit(&session->egress.video_runtime_state, SFU_VIDEO_RUNTIME_FAILED, memory_order_release);
    pthread_mutex_unlock(&session->media.lock);
    return false;
  }

  gcc_bwe_init(gcc, SFU_BWE_START_BPS, SFU_BWE_MIN_BPS, SFU_BWE_MAX_BPS);
  sfu_twcc_history_init(history);
  sfu_twcc_recv_tracker_init(recv);
  session->egress.gcc_ctx = gcc;
  session->egress.twcc_history = history;
  session->egress.twcc_recv = recv;
  session->egress.schedulers = schedulers;
  session->egress.rtx_cache = rtx;
  atomic_store_explicit(&session->egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY, memory_order_release);
  pthread_mutex_unlock(&session->media.lock);
  return true;
}

static uint64_t get_worker_generation(void *ctx, uint32_t worker_index) {
  sfu_worker_t *workers = (sfu_worker_t *)ctx;
  return atomic_load_explicit(&workers[worker_index].generation, memory_order_acquire);
}

static void sfu_session_free_resources(sfu_peer_session_t *s);

static void session_destructor(void *ptr) {
  sfu_peer_session_t *s = (sfu_peer_session_t *)ptr;
  sfu_session_free_resources(s);
  snapshot_wait_unhazarded(s);
  SFU_FREE(s);
}

static void session_table_fail_cleanup(sfu_session_table_t *t) {
  if (t->reclaimer) {
    sfu_epoch_reclaimer_destroy_after_quiescence(t->reclaimer);
    SFU_FREE(t->reclaimer);
    t->reclaimer = NULL;
  }
  SFU_FREE(t->sessions);
  SFU_FREE(t->free_indices);
  t->sessions = NULL;
  t->free_indices = NULL;
  t->capacity = 0;
}

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx, void *workers, uint32_t worker_count) {
  memset(t, 0, sizeof(*t));
  t->capacity = SFU_SESSION_TABLE_MAX;
  t->sessions = SFU_CALLOC(t->capacity, sizeof(*t->sessions));
  t->free_indices = SFU_CALLOC(t->capacity, sizeof(*t->free_indices));

  if (!t->sessions || !t->free_indices) {
    session_table_fail_cleanup(t);
    return -1;
  }

  for (int i = 0; i < SFU_SESSION_ADDR_HASH_SLOTS; i++) {
    t->addr_index[i].index = SFU_HASH_EMPTY;
  }
  for (int i = 0; i < SFU_SESSION_UFRAG_HASH_SLOTS; i++) {
    t->ufrag_index[i].index = SFU_HASH_EMPTY;
  }

  t->dtls_ctx = dtls_ctx;

  if (workers && worker_count > 0) {
    t->reclaimer = SFU_CALLOC(1, sizeof(*t->reclaimer));
    if (!t->reclaimer) {
      session_table_fail_cleanup(t);
      return -1;
    }
    if (sfu_epoch_reclaimer_init(t->reclaimer, worker_count, get_worker_generation, workers) != 0) {
      SFU_FREE(t->reclaimer);
      t->reclaimer = NULL;
      session_table_fail_cleanup(t);
      return -1;
    }
  }

  pthread_rwlockattr_t rwattr;
  if (pthread_rwlockattr_init(&rwattr) != 0) {
    session_table_fail_cleanup(t);
    return -1;
  }
  if (pthread_rwlockattr_setkind_np(&rwattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) != 0) {
    pthread_rwlockattr_destroy(&rwattr);
    session_table_fail_cleanup(t);
    return -1;
  }
  int rwlock_rc = pthread_rwlock_init(&t->lock, &rwattr);
  pthread_rwlockattr_destroy(&rwattr);
  if (rwlock_rc != 0) {
    session_table_fail_cleanup(t);
    return -1;
  }
  if (pthread_mutex_init(&t->ice_lock, NULL) != 0) {
    pthread_rwlock_destroy(&t->lock);
    session_table_fail_cleanup(t);
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

static void receiver_chunk_release(sfu_receiver_chunk_t *chunk) {
  if (!chunk) {
    return;
  }
  uint32_t prev = atomic_fetch_sub_explicit(&chunk->refcount, 1, memory_order_acq_rel);
  assert(prev != 0 && "receiver chunk refcount underflow");
  if (prev != 1) {
    return;
  }
  for (uint32_t i = 0; i < SFU_RECEIVER_CHUNK_SIZE; i++) {
    if (chunk->occupied & (1u << i)) {
      sfu_session_release(chunk->entries[i].subscriber);
    }
  }
  SFU_FREE(chunk);
}

sfu_receiver_snapshot_t *sfu_receiver_snapshot_alloc(void) {
  sfu_receiver_snapshot_t *snap = SFU_CALLOC(1, sizeof(*snap));
  if (!snap) {
    return NULL;
  }
  atomic_store_explicit(&snap->refcount, 1, memory_order_relaxed);
  return snap;
}

const sfu_receiver_entry_t *sfu_receiver_snapshot_at(const sfu_receiver_snapshot_t *snap, uint32_t remote_slot) {
  if (!snap || remote_slot >= SFU_MAX_REMOTE_SLOTS) {
    return NULL;
  }
  sfu_receiver_chunk_t *chunk = snap->chunks[remote_slot >> SFU_RECEIVER_CHUNK_SHIFT];
  uint32_t offset = remote_slot & SFU_RECEIVER_CHUNK_MASK;
  return chunk && (chunk->occupied & (1u << offset)) ? &chunk->entries[offset] : NULL;
}

void sfu_receiver_snapshot_iter_init(sfu_receiver_snapshot_iter_t *iter, const sfu_receiver_snapshot_t *snap) {
  iter->snapshot = snap;
  iter->chunk_index = 0;
  iter->occupied = 0;
}

const sfu_receiver_entry_t *sfu_receiver_snapshot_iter_next(sfu_receiver_snapshot_iter_t *iter, uint32_t *remote_slot) {
  if (!iter || !iter->snapshot) {
    return NULL;
  }
  while (iter->occupied == 0) {
    if (iter->chunk_index >= SFU_RECEIVER_CHUNK_COUNT) {
      return NULL;
    }
    const sfu_receiver_chunk_t *chunk = iter->snapshot->chunks[iter->chunk_index];
    iter->occupied = chunk ? chunk->occupied : 0;
    if (iter->occupied == 0) {
      iter->chunk_index++;
    }
  }

  uint32_t offset = (uint32_t)__builtin_ctz(iter->occupied);
  const sfu_receiver_chunk_t *chunk = iter->snapshot->chunks[iter->chunk_index];
  uint32_t slot = (iter->chunk_index << SFU_RECEIVER_CHUNK_SHIFT) + offset;
  iter->occupied &= iter->occupied - 1;
  if (iter->occupied == 0) {
    iter->chunk_index++;
  }
  if (remote_slot) {
    *remote_slot = slot;
  }
  return &chunk->entries[offset];
}

const sfu_receiver_entry_t *sfu_receiver_snapshot_nth(const sfu_receiver_snapshot_t *snap, uint32_t ordinal, uint32_t *remote_slot) {
  if (!snap || ordinal >= snap->count) {
    return NULL;
  }
  sfu_receiver_snapshot_iter_t iter;
  sfu_receiver_snapshot_iter_init(&iter, snap);
  const sfu_receiver_entry_t *entry;
  while ((entry = sfu_receiver_snapshot_iter_next(&iter, remote_slot)) != NULL) {
    if (ordinal-- == 0) {
      return entry;
    }
  }
  return NULL;
}

const sfu_receiver_entry_t *sfu_receiver_snapshot_find_peer(const sfu_receiver_snapshot_t *snap, const sfu_peer_session_t *peer, uint32_t *remote_slot) {
  sfu_receiver_snapshot_iter_t iter;
  sfu_receiver_snapshot_iter_init(&iter, snap);
  const sfu_receiver_entry_t *entry;
  uint32_t slot;
  while ((entry = sfu_receiver_snapshot_iter_next(&iter, &slot)) != NULL) {
    if (entry->subscriber == peer) {
      if (remote_slot) {
        *remote_slot = slot;
      }
      return entry;
    }
  }
  return NULL;
}

bool sfu_receiver_snapshot_set(sfu_receiver_snapshot_t *snap, uint32_t remote_slot, const sfu_receiver_entry_t *entry) {
  if (!snap || remote_slot >= SFU_MAX_REMOTE_SLOTS) {
    return false;
  }
  uint32_t chunk_index = remote_slot >> SFU_RECEIVER_CHUNK_SHIFT;
  uint32_t offset = remote_slot & SFU_RECEIVER_CHUNK_MASK;
  uint32_t bit = 1u << offset;
  sfu_receiver_chunk_t *chunk = snap->chunks[chunk_index];
  if (!chunk) {
    if (!entry) {
      return true;
    }
    chunk = SFU_CALLOC(1, sizeof(*chunk));
    if (!chunk) {
      return false;
    }
    atomic_store_explicit(&chunk->refcount, 1, memory_order_relaxed);
    snap->chunks[chunk_index] = chunk;
  }
  bool occupied = (chunk->occupied & bit) != 0;
  if (occupied) {
    sfu_session_release(chunk->entries[offset].subscriber);
  }
  if (entry) {
    chunk->entries[offset] = *entry;
    atomic_fetch_add_explicit(&entry->subscriber->refcount, 1, memory_order_relaxed);
    chunk->occupied |= bit;
    if (!occupied) {
      snap->count++;
    }
  } else {
    memset(&chunk->entries[offset], 0, sizeof(chunk->entries[offset]));
    chunk->occupied &= ~bit;
    if (occupied) {
      snap->count--;
    }
  }
  return true;
}

sfu_receiver_snapshot_t *sfu_receiver_snapshot_copy_set(const sfu_receiver_snapshot_t *old, uint32_t remote_slot, const sfu_receiver_entry_t *entry) {
  if (remote_slot >= SFU_MAX_REMOTE_SLOTS) {
    return NULL;
  }
  sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_alloc();
  if (!snap) {
    return NULL;
  }
  if (old) {
    snap->generation = old->generation + 1;
    snap->count = old->count;
    for (uint32_t i = 0; i < SFU_RECEIVER_CHUNK_COUNT; i++) {
      snap->chunks[i] = old->chunks[i];
      if (snap->chunks[i]) {
        atomic_fetch_add_explicit(&snap->chunks[i]->refcount, 1, memory_order_relaxed);
      }
    }
  } else {
    snap->generation = 1;
  }
  uint32_t ci = remote_slot >> SFU_RECEIVER_CHUNK_SHIFT;
  sfu_receiver_chunk_t *shared = snap->chunks[ci];
  if (shared) {
    sfu_receiver_chunk_t *copy = SFU_CALLOC(1, sizeof(*copy));
    if (!copy) {
      sfu_subscriptions_snapshot_release(snap);
      return NULL;
    }
    atomic_store_explicit(&copy->refcount, 1, memory_order_relaxed);
    copy->occupied = shared->occupied;
    memcpy(copy->entries, shared->entries, sizeof(copy->entries));
    for (uint32_t i = 0; i < SFU_RECEIVER_CHUNK_SIZE; i++) {
      if (copy->occupied & (1u << i)) {
        atomic_fetch_add_explicit(&copy->entries[i].subscriber->refcount, 1, memory_order_relaxed);
      }
    }
    snap->chunks[ci] = copy;
    receiver_chunk_release(shared);
  }
  if (!sfu_receiver_snapshot_set(snap, remote_slot, entry)) {
    sfu_subscriptions_snapshot_release(snap);
    return NULL;
  }
  return snap;
}

sfu_receiver_snapshot_t *sfu_session_subscriptions_acquire(const sfu_peer_session_t *s) {
  return s ? snapshot_acquire(s, (const _Atomic(void *) *)&s->graph.receivers) : NULL;
}

void sfu_subscriptions_snapshot_release(sfu_receiver_snapshot_t *snap) {
  if (!snap) {
    return;
  }
  uint32_t prev = atomic_fetch_sub_explicit(&snap->refcount, 1, memory_order_acq_rel);
  assert(prev != 0 && "snapshot refcount underflow");
  if (prev == 1) {
    for (uint32_t i = 0; i < SFU_RECEIVER_CHUNK_COUNT; i++) {
      receiver_chunk_release(snap->chunks[i]);
    }
    SFU_FREE(snap);
  }
}

void sfu_session_publish_receivers(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) {
  pthread_mutex_lock(&owner->graph.lock);
  sfu_receiver_snapshot_t *old = atomic_load_explicit(&owner->graph.receivers, memory_order_acquire);
  atomic_store_explicit(&owner->graph.receivers, new_snap, memory_order_seq_cst);
  pthread_mutex_unlock(&owner->graph.lock);
  snapshot_wait_unhazarded(old);
  sfu_subscriptions_snapshot_release(old);
}

sfu_receiver_snapshot_t *sfu_session_publish_receivers_swap(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) {
  pthread_mutex_lock(&owner->graph.lock);
  sfu_receiver_snapshot_t *old = atomic_load_explicit(&owner->graph.receivers, memory_order_acquire);
  atomic_store_explicit(&owner->graph.receivers, new_snap, memory_order_seq_cst);
  pthread_mutex_unlock(&owner->graph.lock);
  return old;
}

static void fanout_chunk_release(sfu_fanout_chunk_t *chunk) {
  if (!chunk) {
    return;
  }
  uint32_t prev = atomic_fetch_sub_explicit(&chunk->refcount, 1, memory_order_acq_rel);
  assert(prev != 0 && "fanout chunk refcount underflow");
  if (prev != 1) {
    return;
  }
  for (uint32_t i = 0; i < SFU_FANOUT_CHUNK_SIZE; i++) {
    if (chunk->occupied & (1u << i)) {
      sfu_session_release(chunk->routes[i].subscriber);
    }
  }
  SFU_FREE(chunk);
}

sfu_fanout_bundle_t *sfu_fanout_bundle_alloc(void) {
  sfu_fanout_bundle_t *bundle = SFU_CALLOC(1, sizeof(*bundle));
  if (bundle) {
    atomic_store_explicit(&bundle->refcount, 1, memory_order_relaxed);
  }
  return bundle;
}

const sfu_fanout_route_t *sfu_fanout_bundle_at(const sfu_fanout_bundle_t *bundle, uint32_t slot) {
  if (!bundle || slot >= SFU_MAX_REMOTE_SLOTS) {
    return NULL;
  }
  const sfu_fanout_chunk_t *chunk = bundle->chunks[slot >> SFU_FANOUT_CHUNK_SHIFT];
  uint32_t offset = slot & SFU_FANOUT_CHUNK_MASK;
  return chunk && (chunk->occupied & (1u << offset)) ? &chunk->routes[offset] : NULL;
}

const sfu_fanout_route_t *sfu_fanout_bundle_find_peer(const sfu_fanout_bundle_t *bundle, const sfu_peer_session_t *peer, uint32_t *slot) {
  if (!bundle) {
    return NULL;
  }
  for (uint32_t ci = 0; ci < SFU_FANOUT_CHUNK_COUNT; ci++) {
    const sfu_fanout_chunk_t *chunk = bundle->chunks[ci];
    uint32_t occupied = chunk ? chunk->occupied : 0;
    while (occupied) {
      uint32_t offset = (uint32_t)__builtin_ctz(occupied);
      if (chunk->routes[offset].subscriber == peer) {
        if (slot) {
          *slot = (ci << SFU_FANOUT_CHUNK_SHIFT) + offset;
        }
        return &chunk->routes[offset];
      }
      occupied &= occupied - 1;
    }
  }
  return NULL;
}

bool sfu_fanout_bundle_set(sfu_fanout_bundle_t *bundle, uint32_t slot, const sfu_fanout_route_t *route, uint8_t eligibility) {
  if (!bundle || slot >= SFU_MAX_REMOTE_SLOTS) {
    return false;
  }
  uint32_t ci = slot >> SFU_FANOUT_CHUNK_SHIFT, offset = slot & SFU_FANOUT_CHUNK_MASK, bit = 1u << offset;
  sfu_fanout_chunk_t *chunk = bundle->chunks[ci];
  if (!chunk) {
    if (!route) {
      return true;
    }
    chunk = SFU_CALLOC(1, sizeof(*chunk));
    if (!chunk) {
      return false;
    }
    atomic_store_explicit(&chunk->refcount, 1, memory_order_relaxed);
    bundle->chunks[ci] = chunk;
  }
  bool occupied = (chunk->occupied & bit) != 0;
  if (occupied) {
    sfu_session_release(chunk->routes[offset].subscriber);
  }
  if (route) {
    chunk->routes[offset] = *route;
    atomic_fetch_add_explicit(&route->subscriber->refcount, 1, memory_order_relaxed);
    chunk->occupied |= bit;
    if (!occupied) {
      bundle->count++;
    }
  } else {
    memset(&chunk->routes[offset], 0, sizeof(chunk->routes[offset]));
    chunk->occupied &= ~bit;
    if (occupied) {
      bundle->count--;
    }
  }
  chunk->audio_eligible = (chunk->audio_eligible & ~bit) | ((route && (eligibility & SFU_FANOUT_AUDIO)) ? bit : 0);
  chunk->video_eligible = (chunk->video_eligible & ~bit) | ((route && (eligibility & SFU_FANOUT_VIDEO)) ? bit : 0);
  chunk->screen_eligible = (chunk->screen_eligible & ~bit) | ((route && (eligibility & SFU_FANOUT_SCREEN)) ? bit : 0);
  return true;
}

sfu_fanout_bundle_t *sfu_fanout_bundle_copy_set(const sfu_fanout_bundle_t *old, uint32_t slot, const sfu_fanout_route_t *route, uint8_t eligibility) {
  if (slot >= SFU_MAX_REMOTE_SLOTS) {
    return NULL;
  }
  sfu_fanout_bundle_t *bundle = sfu_fanout_bundle_alloc();
  if (!bundle) {
    return NULL;
  }
  if (old) {
    bundle->generation = old->generation + 1;
    bundle->count = old->count;
    for (uint32_t i = 0; i < SFU_FANOUT_CHUNK_COUNT; i++) {
      bundle->chunks[i] = old->chunks[i];
      if (bundle->chunks[i]) {
        atomic_fetch_add_explicit(&bundle->chunks[i]->refcount, 1, memory_order_relaxed);
      }
    }
  } else {
    bundle->generation = 1;
  }
  uint32_t ci = slot >> SFU_FANOUT_CHUNK_SHIFT;
  sfu_fanout_chunk_t *shared = bundle->chunks[ci];
  if (shared) {
    sfu_fanout_chunk_t *copy = SFU_CALLOC(1, sizeof(*copy));
    if (!copy) {
      sfu_fanout_bundle_release(bundle);
      return NULL;
    }
    atomic_store_explicit(&copy->refcount, 1, memory_order_relaxed);
    copy->occupied = shared->occupied;
    copy->audio_eligible = shared->audio_eligible;
    copy->video_eligible = shared->video_eligible;
    copy->screen_eligible = shared->screen_eligible;
    memcpy(copy->routes, shared->routes, sizeof(copy->routes));
    for (uint32_t i = 0; i < SFU_FANOUT_CHUNK_SIZE; i++) {
      if (copy->occupied & (1u << i)) {
        atomic_fetch_add_explicit(&copy->routes[i].subscriber->refcount, 1, memory_order_relaxed);
      }
    }
    bundle->chunks[ci] = copy;
    fanout_chunk_release(shared);
  }
  if (!sfu_fanout_bundle_set(bundle, slot, route, eligibility)) {
    sfu_fanout_bundle_release(bundle);
    return NULL;
  }
  return bundle;
}

void sfu_fanout_iter_init(sfu_fanout_iter_t *iter, const sfu_fanout_bundle_t *bundle, sfu_media_kind_t kind) {
  iter->bundle = bundle;
  iter->chunk_index = 0;
  iter->eligible = 0;
  iter->kind = kind;
}

const sfu_fanout_route_t *sfu_fanout_iter_next(sfu_fanout_iter_t *iter, uint32_t *slot) {
  if (!iter || !iter->bundle) {
    return NULL;
  }
  while (!iter->eligible) {
    if (iter->chunk_index >= SFU_FANOUT_CHUNK_COUNT) {
      return NULL;
    }
    const sfu_fanout_chunk_t *chunk = iter->bundle->chunks[iter->chunk_index];
    iter->eligible = !chunk                           ? 0
                     : iter->kind == SFU_MEDIA_AUDIO  ? chunk->audio_eligible
                     : iter->kind == SFU_MEDIA_SCREEN ? chunk->screen_eligible
                                                      : chunk->video_eligible;
    if (!iter->eligible) {
      iter->chunk_index++;
    }
  }
  uint32_t offset = (uint32_t)__builtin_ctz(iter->eligible);
  const sfu_fanout_chunk_t *chunk = iter->bundle->chunks[iter->chunk_index];
  uint32_t absolute = (iter->chunk_index << SFU_FANOUT_CHUNK_SHIFT) + offset;
  iter->eligible &= iter->eligible - 1;
  if (!iter->eligible) {
    iter->chunk_index++;
  }
  if (slot) {
    *slot = absolute;
  }
  return &chunk->routes[offset];
}

sfu_fanout_bundle_t *sfu_session_fanout_acquire(const sfu_peer_session_t *s) {
  return s ? snapshot_acquire(s, (const _Atomic(void *) *)&s->graph.fanout_bundle) : NULL;
}

void sfu_fanout_bundle_release(sfu_fanout_bundle_t *bundle) {
  if (!bundle) {
    return;
  }
  uint32_t prev = atomic_fetch_sub_explicit(&bundle->refcount, 1, memory_order_acq_rel);
  assert(prev != 0 && "fanout bundle refcount underflow");
  if (prev != 1) {
    return;
  }
  for (uint32_t i = 0; i < SFU_FANOUT_CHUNK_COUNT; i++) {
    fanout_chunk_release(bundle->chunks[i]);
  }
  SFU_FREE(bundle);
}

sfu_fanout_bundle_t *sfu_session_publish_fanout_swap(sfu_peer_session_t *owner, sfu_fanout_bundle_t *bundle) {
  pthread_mutex_lock(&owner->graph.lock);
  sfu_fanout_bundle_t *old = atomic_load_explicit(&owner->graph.fanout_bundle, memory_order_relaxed);
  atomic_store_explicit(&owner->graph.fanout_bundle, bundle, memory_order_seq_cst);
  pthread_mutex_unlock(&owner->graph.lock);
  return old;
}

void sfu_session_publish_fanout(sfu_peer_session_t *owner, sfu_fanout_bundle_t *bundle) {
  sfu_fanout_bundle_t *old = sfu_session_publish_fanout_swap(owner, bundle);
  snapshot_wait_unhazarded(old);
  sfu_fanout_bundle_release(old);
}

void sfu_snapshot_reclaim_receivers(sfu_receiver_snapshot_t *old) {
  if (!old) {
    return;
  }
  snapshot_wait_unhazarded(old);
  sfu_subscriptions_snapshot_release(old);
}

void sfu_snapshot_reclaim_fanout(sfu_fanout_bundle_t *old) {
  if (!old) {
    return;
  }
  snapshot_wait_unhazarded(old);
  sfu_fanout_bundle_release(old);
}

void sfu_remote_offer_manifest_retain(sfu_remote_offer_manifest_t *manifest) {
  if (manifest) {
    atomic_fetch_add_explicit(&manifest->refcount, 1, memory_order_relaxed);
  }
}

void sfu_remote_offer_manifest_release(sfu_remote_offer_manifest_t *manifest) {
  if (manifest && atomic_fetch_sub_explicit(&manifest->refcount, 1, memory_order_acq_rel) == 1) {
    sfu_subscriptions_snapshot_release(manifest->receiver_root);
    SFU_FREE(manifest);
  }
}

static uint64_t remote_slot_next_nonzero(uint64_t *counter) {
  uint64_t generation = ++*counter;
  if (generation == 0) {
    generation = ++*counter;
  }
  return generation;
}

bool sfu_session_remote_slot_reserve(sfu_peer_session_t *session, int64_t publisher_user_id, uint32_t publisher_peer_id, uint32_t *slot,
                                     uint64_t *assignment_generation) {
  if (!session || publisher_peer_id == 0) {
    return false;
  }
  pthread_mutex_lock(&session->graph.lock);
  sfu_remote_slot_table_t *table = &session->graph.remote_slots;
  uint32_t free_slot = UINT32_MAX;
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    sfu_remote_slot_t *candidate = &table->slots[i];
    if (candidate->state == SFU_REMOTE_SLOT_ACTIVE && candidate->publisher_peer_id == publisher_peer_id && candidate->publisher_user_id == publisher_user_id) {
      if (slot) {
        *slot = i;
      }
      if (assignment_generation) {
        *assignment_generation = candidate->assignment_generation;
      }
      pthread_mutex_unlock(&session->graph.lock);
      return true;
    }
    if (free_slot == UINT32_MAX && candidate->state == SFU_REMOTE_SLOT_FREE) {
      free_slot = i;
    }
  }
  if (free_slot == UINT32_MAX) {
    pthread_mutex_unlock(&session->graph.lock);
    return false;
  }
  sfu_remote_slot_t *reserved = &table->slots[free_slot];
  reserved->assignment_generation = remote_slot_next_nonzero(&table->next_assignment_generation);
  reserved->publisher_user_id = publisher_user_id;
  reserved->publisher_peer_id = publisher_peer_id;
  reserved->state = SFU_REMOTE_SLOT_ACTIVE;
  if (free_slot >= table->high_water_slots) {
    table->high_water_slots = free_slot + 1;
  }
  if (slot) {
    *slot = free_slot;
  }
  if (assignment_generation) {
    *assignment_generation = reserved->assignment_generation;
  }
  pthread_mutex_unlock(&session->graph.lock);
  return true;
}

bool sfu_session_remote_slot_retire(sfu_peer_session_t *session, uint32_t slot, uint64_t assignment_generation) {
  if (!session || slot >= SFU_MAX_REMOTE_SLOTS || assignment_generation == 0) {
    return false;
  }
  pthread_mutex_lock(&session->graph.lock);
  sfu_remote_slot_t *remote = &session->graph.remote_slots.slots[slot];
  bool matched = remote->state == SFU_REMOTE_SLOT_ACTIVE && remote->assignment_generation == assignment_generation;
  if (matched) {
    uint64_t applied = atomic_load_explicit(&session->graph.remote_slots.applied_assignment_generations[slot], memory_order_acquire);
    bool offered =
        session->graph.remote_slots.offered_manifest && session->graph.remote_slots.offered_manifest->assignment_generations[slot] == assignment_generation;
    if (offered) {
      remote->state = SFU_REMOTE_SLOT_RETIRING_OFFERED;
    } else if (applied == assignment_generation) {
      remote->state = SFU_REMOTE_SLOT_RETIRING;
    } else {
      memset(remote, 0, sizeof(*remote));
    }
  }
  pthread_mutex_unlock(&session->graph.lock);
  return matched;
}

sfu_remote_offer_manifest_t *sfu_session_remote_offer_capture(sfu_peer_session_t *session) {
  if (!session) {
    return NULL;
  }
  sfu_remote_offer_manifest_t *manifest = SFU_CALLOC(1, sizeof(*manifest));
  if (!manifest) {
    return NULL;
  }
  atomic_store_explicit(&manifest->refcount, 1, memory_order_relaxed);

  pthread_mutex_lock(&session->graph.lock);
  sfu_remote_slot_table_t *table = &session->graph.remote_slots;
  manifest->offer_generation = remote_slot_next_nonzero(&table->next_offer_generation);
  manifest->high_water_slots = table->high_water_slots > table->offered_slot_floor ? table->high_water_slots : table->offered_slot_floor;
  manifest->receiver_root = atomic_load_explicit(&session->graph.receivers, memory_order_acquire);
  if (manifest->receiver_root) {
    atomic_fetch_add_explicit(&manifest->receiver_root->refcount, 1, memory_order_relaxed);
  }
  for (uint32_t i = 0; i < manifest->high_water_slots; i++) {
    if (table->slots[i].state == SFU_REMOTE_SLOT_ACTIVE) {
      manifest->assignment_generations[i] = table->slots[i].assignment_generation;
    }
  }
  pthread_mutex_unlock(&session->graph.lock);
  return manifest;
}

bool sfu_session_remote_offer_install(sfu_peer_session_t *session, sfu_remote_offer_manifest_t *manifest) {
  if (!session || !manifest || manifest->offer_generation == 0) {
    return false;
  }
  pthread_mutex_lock(&session->graph.lock);
  sfu_remote_slot_table_t *table = &session->graph.remote_slots;
  if (table->offered_manifest && table->offered_manifest->offer_generation >= manifest->offer_generation) {
    pthread_mutex_unlock(&session->graph.lock);
    return false;
  }
  for (uint32_t i = 0; i < table->high_water_slots; i++) {
    if (table->slots[i].state == SFU_REMOTE_SLOT_RETIRING && (i >= manifest->high_water_slots || manifest->assignment_generations[i] == 0)) {
      table->slots[i].state = SFU_REMOTE_SLOT_RETIRING_OFFERED;
    }
  }
  sfu_remote_offer_manifest_t *old = table->offered_manifest;
  table->offered_manifest = manifest;
  if (manifest->high_water_slots > table->offered_slot_floor) {
    table->offered_slot_floor = manifest->high_water_slots;
  }
  sfu_remote_offer_manifest_retain(manifest);
  pthread_mutex_unlock(&session->graph.lock);
  sfu_remote_offer_manifest_release(old);
  return true;
}

sfu_remote_offer_manifest_t *sfu_session_remote_offer_acquire_current(sfu_peer_session_t *session) {
  if (!session) {
    return NULL;
  }
  pthread_mutex_lock(&session->graph.lock);
  sfu_remote_offer_manifest_t *manifest = session->graph.remote_slots.offered_manifest;
  sfu_remote_offer_manifest_retain(manifest);
  pthread_mutex_unlock(&session->graph.lock);
  return manifest;
}

bool sfu_session_remote_offer_apply_answer(sfu_peer_session_t *session, const sfu_remote_offer_manifest_t *manifest) {
  if (!session || !manifest || manifest->offer_generation == 0) {
    return false;
  }
  pthread_mutex_lock(&session->graph.lock);
  sfu_remote_slot_table_t *table = &session->graph.remote_slots;
  if (table->offered_manifest != manifest) {
    pthread_mutex_unlock(&session->graph.lock);
    return false;
  }
  for (uint32_t i = 0; i < manifest->high_water_slots; i++) {
    uint64_t offered = manifest->assignment_generations[i];
    sfu_remote_slot_t *remote = &table->slots[i];
    if (offered != 0 && remote->assignment_generation != offered) {
      pthread_mutex_unlock(&session->graph.lock);
      return false;
    }
  }
  for (uint32_t i = 0; i < table->high_water_slots; i++) {
    uint64_t offered = i < manifest->high_water_slots ? manifest->assignment_generations[i] : 0;
    atomic_store_explicit(&table->applied_assignment_generations[i], offered, memory_order_release);
#ifdef SFU_DIAG_LOG
    if (offered != 0 || table->slots[i].state != SFU_REMOTE_SLOT_FREE) {
      SFU_LOG_INFO("session: apply_answer peer=%u slot=%u offered_gen=%" PRIu64 " slot_gen=%" PRIu64 " state=%d offer_gen=%" PRIu64, session->peer_id, i,
                   offered, table->slots[i].assignment_generation, (int)table->slots[i].state, manifest->offer_generation);
    }
#endif
    sfu_remote_slot_t *remote = &table->slots[i];
    if (offered == 0 && (remote->state == SFU_REMOTE_SLOT_RETIRING || remote->state == SFU_REMOTE_SLOT_RETIRING_OFFERED)) {
      memset(remote, 0, sizeof(*remote));
    } else if (offered != 0 && (remote->state == SFU_REMOTE_SLOT_RETIRING || remote->state == SFU_REMOTE_SLOT_RETIRING_OFFERED)) {
      remote->state = SFU_REMOTE_SLOT_RETIRING;
    }
  }
  while (table->high_water_slots > 0) {
    uint32_t last = table->high_water_slots - 1;
    if (table->slots[last].state != SFU_REMOTE_SLOT_FREE) {
      break;
    }
    if (atomic_load_explicit(&table->applied_assignment_generations[last], memory_order_acquire) != 0) {
      break;
    }
    table->high_water_slots = last;
  }
  table->offered_manifest = NULL;
  pthread_mutex_unlock(&session->graph.lock);
  sfu_remote_offer_manifest_release((sfu_remote_offer_manifest_t *)manifest);
  return true;
}

bool sfu_session_remote_slot_authorized(const sfu_peer_session_t *session, uint32_t slot, uint64_t assignment_generation) {
  return session && slot < SFU_MAX_REMOTE_SLOTS && assignment_generation != 0 &&
         atomic_load_explicit(&session->graph.remote_slots.applied_assignment_generations[slot], memory_order_acquire) == assignment_generation;
}

bool sfu_session_remote_slots_pending(const sfu_peer_session_t *session, uint32_t *active_unapplied, uint32_t *obsolete_applied) {
  uint32_t active_count = 0;
  uint32_t obsolete_count = 0;
  if (session) {
    pthread_mutex_lock((pthread_mutex_t *)&session->graph.lock);
    const sfu_remote_slot_table_t *table = &session->graph.remote_slots;
    for (uint32_t i = 0; i < table->high_water_slots; i++) {
      const sfu_remote_slot_t *slot = &table->slots[i];
      uint64_t applied = atomic_load_explicit(&table->applied_assignment_generations[i], memory_order_acquire);
      if (slot->state == SFU_REMOTE_SLOT_ACTIVE) {
        if (applied != slot->assignment_generation) {
          active_count++;
        }
      } else if (applied != 0) {
        obsolete_count++;
      }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&session->graph.lock);
  }
  if (active_unapplied) {
    *active_unapplied = active_count;
  }
  if (obsolete_applied) {
    *obsolete_applied = obsolete_count;
  }
  return active_count != 0 || obsolete_count != 0;
}

uint32_t sfu_session_remote_slot_high_water(const sfu_peer_session_t *session) {
  if (!session) {
    return 0;
  }
  pthread_mutex_lock((pthread_mutex_t *)&session->graph.lock);
  uint32_t high_water = session->graph.remote_slots.high_water_slots;
  if (session->graph.remote_slots.offered_slot_floor > high_water) {
    high_water = session->graph.remote_slots.offered_slot_floor;
  }
  pthread_mutex_unlock((pthread_mutex_t *)&session->graph.lock);
  return high_water;
}

void sfu_session_remote_slots_teardown(sfu_peer_session_t *session) {
  if (!session) {
    return;
  }
  pthread_mutex_lock(&session->graph.lock);
  sfu_remote_offer_manifest_t *manifest = session->graph.remote_slots.offered_manifest;
  session->graph.remote_slots.offered_manifest = NULL;
  memset(session->graph.remote_slots.slots, 0, sizeof(session->graph.remote_slots.slots));
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    atomic_store_explicit(&session->graph.remote_slots.applied_assignment_generations[i], 0, memory_order_release);
  }
  session->graph.remote_slots.high_water_slots = 0;
  session->graph.remote_slots.offered_slot_floor = 0;
  pthread_mutex_unlock(&session->graph.lock);
  sfu_remote_offer_manifest_release(manifest);
}

void sfu_session_graph_assert_invariants(const sfu_peer_session_t *session) {
#ifndef NDEBUG
  if (!session) {
    return;
  }
  pthread_mutex_lock((pthread_mutex_t *)&session->graph.lock);
  sfu_room_t *room = session->room;
  if (room) {
    assert(session->room_slot < SFU_ROOM_MAX_PEERS);
    assert(session->room_slot < room->peer_capacity);
    assert(room->occupied[session->room_slot]);
    assert(room->peers[session->room_slot] == session);
  } else {
    assert(session->room_slot == UINT32_MAX);
  }

  const sfu_remote_slot_table_t *table = &session->graph.remote_slots;
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    const sfu_remote_slot_t *slot = &table->slots[i];
    uint64_t applied = atomic_load_explicit(&table->applied_assignment_generations[i], memory_order_acquire);
    if (slot->state == SFU_REMOTE_SLOT_FREE) {
      assert(applied == 0);
      continue;
    }
    assert(slot->assignment_generation != 0);
    for (uint32_t j = i + 1; j < SFU_MAX_REMOTE_SLOTS; j++) {
      const sfu_remote_slot_t *other = &table->slots[j];
      if (other->state != SFU_REMOTE_SLOT_ACTIVE || slot->state != SFU_REMOTE_SLOT_ACTIVE) {
        continue;
      }
      assert(!(other->publisher_user_id == slot->publisher_user_id && other->publisher_peer_id == slot->publisher_peer_id));
    }
    if (slot->state == SFU_REMOTE_SLOT_RETIRING || slot->state == SFU_REMOTE_SLOT_RETIRING_OFFERED) {
      if (applied != 0) {
        assert(applied == slot->assignment_generation);
      }
    }
  }

  sfu_receiver_snapshot_t *subs = atomic_load_explicit(&session->graph.receivers, memory_order_acquire);
  if (subs) {
    sfu_receiver_snapshot_iter_t iter;
    sfu_receiver_snapshot_iter_init(&iter, subs);
    const sfu_receiver_entry_t *entry;
    uint32_t remote_slot;
    while ((entry = sfu_receiver_snapshot_iter_next(&iter, &remote_slot)) != NULL) {
      assert(remote_slot < SFU_MAX_REMOTE_SLOTS);
      assert(entry->remote_slot == 0 || entry->remote_slot == remote_slot);
      assert(entry->assignment_generation != 0);
      assert(table->slots[remote_slot].state != SFU_REMOTE_SLOT_FREE);
      if (table->slots[remote_slot].state == SFU_REMOTE_SLOT_ACTIVE) {
        assert(table->slots[remote_slot].assignment_generation == entry->assignment_generation);
      }
    }
  }

  sfu_fanout_bundle_t *bundle = atomic_load_explicit(&session->graph.fanout_bundle, memory_order_acquire);
  if (bundle) {
    atomic_fetch_add_explicit(&bundle->refcount, 1, memory_order_relaxed);
  }
  pthread_mutex_unlock((pthread_mutex_t *)&session->graph.lock);

  if (bundle) {
    for (uint32_t slot = 0; slot < SFU_MAX_REMOTE_SLOTS; slot++) {
      const sfu_fanout_route_t *route = sfu_fanout_bundle_at(bundle, slot);
      if (!route) {
        continue;
      }
      assert(route->subscriber);
      assert(route->assignment_generation != 0);
      sfu_receiver_snapshot_t *peer_subs = sfu_session_subscriptions_acquire(route->subscriber);
      const sfu_receiver_entry_t *peer_entry = sfu_receiver_snapshot_find_peer(peer_subs, session, NULL);
      assert(peer_entry);
      assert(peer_entry->assignment_generation == route->assignment_generation);
      sfu_subscriptions_snapshot_release(peer_subs);
    }
    sfu_fanout_bundle_release(bundle);
  }
#else
  (void)session;
#endif
}

static void sfu_session_free_resources(sfu_peer_session_t *s) {
  if (!s) {
    return;
  }

  if (s->active) {
    pthread_mutex_lock(&s->crypto_lock);
    sfu_srtp_ctx_destroy(&s->srtp);
    sfu_srtp_ctx_destroy(&s->previous_srtp);
    pthread_mutex_unlock(&s->crypto_lock);
    if (s->cold) {
      sfu_dtls_conn_destroy(&s->cold->dtls);
      sfu_dtls_conn_destroy(&s->cold->pending_dtls);
    }
  }

  sfu_session_remote_slots_teardown(s);

  pthread_mutex_lock(&s->graph.lock);
  sfu_receiver_snapshot_t *snap = atomic_load_explicit(&s->graph.receivers, memory_order_acquire);
  atomic_store_explicit(&s->graph.receivers, NULL, memory_order_release);
  pthread_mutex_unlock(&s->graph.lock);
  snapshot_wait_unhazarded(snap);
  sfu_subscriptions_snapshot_release(snap);

  pthread_mutex_lock(&s->graph.lock);
  sfu_fanout_bundle_t *fanout = atomic_load_explicit(&s->graph.fanout_bundle, memory_order_relaxed);
  atomic_store_explicit(&s->graph.fanout_bundle, NULL, memory_order_seq_cst);
  pthread_mutex_unlock(&s->graph.lock);
  snapshot_wait_unhazarded(fanout);
  sfu_fanout_bundle_release(fanout);

  if (s->egress.rtx_cache) {
    sfu_rtx_cache_destroy(s->egress.rtx_cache);
    SFU_FREE(s->egress.rtx_cache);
    s->egress.rtx_cache = NULL;
  }
  if (s->egress.gcc_ctx) {
    SFU_FREE(s->egress.gcc_ctx);
    s->egress.gcc_ctx = NULL;
  }
  if (s->egress.twcc_history) {
    SFU_FREE(s->egress.twcc_history);
    s->egress.twcc_history = NULL;
  }
  if (s->egress.twcc_recv) {
    SFU_FREE(s->egress.twcc_recv);
    s->egress.twcc_recv = NULL;
  }
  if (s->egress.schedulers) {
    SFU_FREE(s->egress.schedulers);
    s->egress.schedulers = NULL;
  }
  if (s->leave_event) {
    assert(!atomic_load_explicit(&s->leave_event_in_use, memory_order_acquire));
    SFU_FREE(s->leave_event);
    s->leave_event = NULL;
  }
  if (s->cold) {
    SFU_FREE(s->cold);
    s->cold = NULL;
  }
  pthread_mutex_destroy(&s->membership_lock);
  pthread_mutex_destroy(&s->ingress_lock);
  pthread_mutex_destroy(&s->crypto_lock);
  pthread_mutex_destroy(&s->graph.lock);
  pthread_mutex_destroy(&s->media.lock);
  pthread_mutex_destroy(&s->negotiation.lock);
  pthread_mutex_destroy(&s->answer_lock);
}

void sfu_session_release(sfu_peer_session_t *s) {
  if (!s) {
    return;
  }

  if (atomic_fetch_sub_explicit(&s->refcount, 1, memory_order_acq_rel) == 1) {
    sfu_session_table_t *table = s->cold ? (sfu_session_table_t *)s->cold->table : NULL;
    if (table && table->reclaimer) {
      unsigned spins = 0;
      while (!sfu_epoch_reclaimer_retire(table->reclaimer, s, session_destructor)) {
        (void)sfu_epoch_reclaimer_sweep(table->reclaimer);
        if ((++spins % 64u) == 0u) {
          SFU_LOG_WARN("epoch reclaimer full, waiting for worker grace period (spins=%u)", spins);
        }
        sched_yield();
      }
    } else {
      sfu_session_free_resources(s);
      snapshot_wait_unhazarded(s);
      SFU_FREE(s);
    }
  }
}

static bool addr_equal(const struct sockaddr_storage *a, socklen_t a_len, const struct sockaddr_storage *b, socklen_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  return memcmp(a, b, a_len) == 0;
}

static uint32_t table_member_index(const sfu_session_table_t *t, const sfu_peer_session_t *session) {
  if (!session->cold) {
    return UINT32_MAX;
  }
  uint32_t index = session->cold->table_index;
  return index < t->count && t->sessions[index] == session ? index : UINT32_MAX;
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

static bool table_index_addr_locked(sfu_session_table_t *t, sfu_peer_session_t *session, uint32_t idx) {
  uint32_t hash = fnv1a(&session->cold->addr, session->cold->addr_len);
  addr_match_ctx_t ctx = {t, &session->cold->addr, session->cold->addr_len};
  uint32_t slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, hash, addr_matches_direct, &ctx, true);
  if (slot == SFU_HASH_EMPTY) {
    return false;
  }
  t->addr_index[slot].hash = hash;
  t->addr_index[slot].index = idx;
  return true;
}

static void table_rollback_reserved_slot(sfu_session_table_t *t, uint32_t index, bool reused) {
  if (reused) {
    t->free_indices[t->free_count++] = index;
  } else {
    assert(index + 1 == t->count);
    t->count--;
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

  uint32_t addr_hash = fnv1a(addr, addr_len);
  addr_match_ctx_t addr_ctx = {t, addr, addr_len};
  uint32_t addr_slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, addr_hash, addr_matches_direct, &addr_ctx, false);
  if (addr_slot != SFU_HASH_EMPTY) {
    uint32_t existing_index = t->addr_index[addr_slot].index;
    sfu_peer_session_t *session = existing_index < t->count ? t->sessions[existing_index] : NULL;
    if (session) {
      atomic_fetch_add_explicit(&session->refcount, 1, memory_order_relaxed);
      pthread_rwlock_unlock(&t->lock);
      return session;
    }
  }

  bool reused = t->free_count > 0;
  uint32_t index;
  if (reused) {
    index = t->free_indices[--t->free_count];
  } else {
    if (t->count >= t->capacity) {
      SFU_LOG_WARN("session table full (%u), rejecting new peer", t->capacity);
      pthread_rwlock_unlock(&t->lock);
      return NULL;
    }
    index = t->count++;
  }

  sfu_peer_session_t *s = SFU_CALLOC(1, sizeof(sfu_peer_session_t));
  if (!s) {
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  s->cold = SFU_CALLOC(1, sizeof(sfu_peer_session_cold_t));
  if (!s->cold) {
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  s->cold->table = t;
  s->cold->table_index = UINT32_MAX;
  s->room_slot = UINT32_MAX;
  memcpy(&s->cold->addr, addr, addr_len);
  s->cold->addr_len = addr_len;
  s->active = true;
  s->state = SFU_SESSION_NEW;
  s->fd = -1;
  atomic_store_explicit(&s->worker_owner, SFU_SESSION_OWNER_NONE, memory_order_relaxed);
  if (pthread_mutex_init(&s->answer_lock, NULL) != 0) {
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->negotiation.lock, NULL) != 0) {
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->media.lock, NULL) != 0) {
    pthread_mutex_destroy(&s->negotiation.lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->graph.lock, NULL) != 0) {
    pthread_mutex_destroy(&s->media.lock);
    pthread_mutex_destroy(&s->negotiation.lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->crypto_lock, NULL) != 0) {
    pthread_mutex_destroy(&s->graph.lock);
    pthread_mutex_destroy(&s->media.lock);
    pthread_mutex_destroy(&s->negotiation.lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->ingress_lock, NULL) != 0) {
    pthread_mutex_destroy(&s->crypto_lock);
    pthread_mutex_destroy(&s->graph.lock);
    pthread_mutex_destroy(&s->media.lock);
    pthread_mutex_destroy(&s->negotiation.lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  if (pthread_mutex_init(&s->membership_lock, NULL) != 0) {
    pthread_mutex_destroy(&s->ingress_lock);
    pthread_mutex_destroy(&s->crypto_lock);
    pthread_mutex_destroy(&s->graph.lock);
    pthread_mutex_destroy(&s->media.lock);
    pthread_mutex_destroy(&s->negotiation.lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  s->leave_event = SFU_CALLOC(1, sizeof(*s->leave_event));
  if (!s->leave_event) {
    pthread_mutex_destroy(&s->membership_lock);
    pthread_mutex_destroy(&s->ingress_lock);
    pthread_mutex_destroy(&s->crypto_lock);
    pthread_mutex_destroy(&s->graph.lock);
    pthread_mutex_destroy(&s->media.lock);
    pthread_mutex_destroy(&s->negotiation.lock);
    pthread_mutex_destroy(&s->answer_lock);
    SFU_FREE(s->cold);
    SFU_FREE(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  s->leave_event->preallocated_storage = true;
  s->leave_event->storage_owner = s;
  atomic_store_explicit(&s->leave_event_in_use, false, memory_order_relaxed);

  for (int i = 0; i < 128; i++) {
    s->media.pt_map[i] = (uint8_t)i;
  }

  s->media.uplink_audio.owner = s;
  s->media.uplink_audio.mid = SFU_LOCAL_AUDIO_MID;
  s->media.uplink_audio.kind = SFU_MEDIA_AUDIO;
  s->media.uplink_video.owner = s;
  s->media.uplink_video.mid = SFU_LOCAL_CAMERA_MID;
  s->media.uplink_video.kind = SFU_MEDIA_VIDEO;
  s->media.screen.owner = s;
  s->media.screen.mid = SFU_LOCAL_SCREEN_MID;
  s->media.screen.kind = SFU_MEDIA_SCREEN;

  atomic_store_explicit(&s->graph.receivers, NULL, memory_order_relaxed);
  atomic_store_explicit(&s->graph.fanout_bundle, NULL, memory_order_relaxed);
  atomic_store_explicit(&s->is_audience, false, memory_order_relaxed);
  atomic_store_explicit(&s->screen_codec_preference, SFU_VIDEO_CODEC_NONE, memory_order_relaxed);
  atomic_store_explicit(&s->media.ptt_active, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.camera_enabled, true, memory_order_relaxed);
  atomic_store_explicit(&s->media.screen_enabled, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.camera_rtp_observed, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.screen_rtp_observed, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.media_update_queued, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.camera_announced_active, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.screen_announced_active, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.audio_send_negotiated, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.video_send_negotiated, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.screen_send_negotiated, false, memory_order_relaxed);
  atomic_store_explicit(&s->media.visible, true, memory_order_relaxed);

  /* Initialize the seqlock-protected media snapshot to match the zeroed transceivers. */
  atomic_store_explicit(&s->media.snapshot_words[0], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[1], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[2], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[3], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[4], 0, memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_seq, 0, memory_order_relaxed);

  {
    static atomic_uint_fast32_t peer_id_counter = 0;
    uint32_t id = (uint32_t)atomic_fetch_add_explicit(&peer_id_counter, 1, memory_order_relaxed) + 1;
    if (id == 0) {
      id = (uint32_t)atomic_fetch_add_explicit(&peer_id_counter, 1, memory_order_relaxed) + 1;
    }
    s->peer_id = id;
  }

  atomic_store_explicit(&s->egress.video_runtime_state, SFU_VIDEO_RUNTIME_UNINITIALIZED, memory_order_relaxed);
  sfu_rtp_seq_translator_init(&s->cold->rtp_seq_translator);
  sfu_pacer_init(&s->egress.pacer);
  sfu_pacer_set_rate(&s->egress.pacer, SFU_BWE_START_BPS, (int64_t)sfu_now_us());

  if (sfu_dtls_conn_init(&s->cold->dtls, t->dtls_ctx) != 0) {
    SFU_LOG_ERROR("failed to init DTLS connection for new peer session");
    session_destroy_unpublished(s);
    table_rollback_reserved_slot(t, index, reused);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }

  atomic_store_explicit(&s->refcount, 2, memory_order_relaxed);
  atomic_store_explicit(&s->lifecycle, SFU_SESSION_LIFECYCLE_OPEN, memory_order_relaxed);
  atomic_store_explicit(&s->accepts_work, true, memory_order_relaxed);

  s->cold->table_index = index;
  t->sessions[index] = s;
  if (!table_index_addr_locked(t, s, index)) {
    t->sessions[index] = NULL;
    s->cold->table_index = UINT32_MAX;
    table_rollback_reserved_slot(t, index, reused);
    session_destroy_unpublished(s);
    pthread_rwlock_unlock(&t->lock);
    return NULL;
  }
  t->active_count++;

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

  /* Membership is the outer lifecycle lock: admission/removal take it before a
   * room lock, and close takes it before the session-table lock.  Never wait
   * for membership while holding the table lock. */
  pthread_mutex_lock(&s->membership_lock);
  pthread_rwlock_wrlock(&t->lock);

  if (atomic_load_explicit(&s->lifecycle, memory_order_acquire) != SFU_SESSION_LIFECYCLE_OPEN) {
    pthread_rwlock_unlock(&t->lock);
    pthread_mutex_unlock(&s->membership_lock);
    return false;
  }

  atomic_store_explicit(&s->lifecycle, SFU_SESSION_LIFECYCLE_CLOSING, memory_order_release);
  atomic_store_explicit(&s->accepts_work, false, memory_order_release);

  uint32_t idx = table_member_index(t, s);
  if (idx != UINT32_MAX) {
    table_remove_addr_hash(t, s, idx);
    table_remove_ufrag_hash(t, s, idx);
    t->sessions[idx] = NULL;
    s->cold->table_index = UINT32_MAX;
    t->free_indices[t->free_count++] = idx;
    if (t->active_count > 0) {
      t->active_count--;
    }
  }

  pthread_rwlock_unlock(&t->lock);

  sfu_room_t *room = (sfu_room_t *)s->room;
  if (room) {
    room_remove_peer_membership_locked(room, s);
  }
  pthread_mutex_unlock(&s->membership_lock);

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

  uint32_t insert_slot = addr_probe(t->addr_index, SFU_SESSION_ADDR_HASH_SLOTS, new_hash, addr_matches_direct, &new_ctx, true);
  if (insert_slot == SFU_HASH_EMPTY) {
    pthread_rwlock_unlock(&t->lock);
    return false;
  }

  table_remove_addr_hash(t, s, idx);
  memcpy(&s->cold->addr, addr, addr_len);
  s->cold->addr_len = addr_len;
  t->addr_index[insert_slot].hash = new_hash;
  t->addr_index[insert_slot].index = idx;

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
  for (;;) {
    sfu_peer_session_t *orphan = NULL;

    /* Pin a table member, then close it through the normal membership-first
     * protocol.  Taking membership while the table lock is held would invert
     * begin_close's ordering and can deadlock with room admission. */
    pthread_rwlock_rdlock(&t->lock);
    for (uint32_t i = 0; i < t->count; i++) {
      orphan = t->sessions[i];
      if (orphan) {
        atomic_fetch_add_explicit(&orphan->refcount, 1, memory_order_relaxed);
        break;
      }
    }
    pthread_rwlock_unlock(&t->lock);

    if (!orphan) {
      break;
    }
    (void)sfu_session_begin_close(t, orphan);
    sfu_session_release(orphan);
  }

  if (t->reclaimer) {
    sfu_epoch_reclaimer_destroy_after_quiescence(t->reclaimer);
    SFU_FREE(t->reclaimer);
    t->reclaimer = NULL;
  }

  SFU_FREE(t->sessions);
  SFU_FREE(t->free_indices);
  t->sessions = NULL;
  t->free_indices = NULL;
  t->count = 0;
  t->active_count = 0;
  t->free_count = 0;
  t->capacity = 0;

  pthread_mutex_destroy(&t->ice_lock);
  pthread_rwlock_destroy(&t->lock);
}

static uint32_t sfu_resolve_media_ssrc(uint32_t live, uint32_t live_rtx, uint32_t answer_ssrc, uint32_t answer_rtx) {
  uint32_t rtx = answer_rtx != 0 ? answer_rtx : live_rtx;
  uint32_t ssrc = answer_ssrc;
  if (live != 0 && live != rtx) {
    ssrc = live;
  }
  if (ssrc != 0 && rtx != 0 && ssrc == rtx) {
    ssrc = (answer_ssrc != 0 && answer_ssrc != rtx) ? answer_ssrc : 0;
  }
  return ssrc;
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

  if ((answer->video_sends || answer->screen_sends) && !answer->is_audience && !sfu_session_ensure_video_runtime(session)) {
    SFU_LOG_ERROR("session %u: failed to initialize video runtime", session->peer_id);
    pthread_mutex_unlock(&session->answer_lock);
    return false;
  }

  if (answer->audio_section_present) {
    atomic_store_explicit(&session->media.audio_send_negotiated, answer->audio_sends, memory_order_release);
  }
  if (answer->video_section_present) {
    atomic_store_explicit(&session->media.video_send_negotiated, answer->video_sends && !answer->is_audience, memory_order_release);
  }
  if (answer->screen_section_present) {
    atomic_store_explicit(&session->media.screen_send_negotiated, answer->screen_sends && !answer->is_audience, memory_order_release);
  }

  uint32_t audio_ssrc = 0;
  uint32_t video_ssrc = 0;
  uint32_t screen_ssrc = 0;
  bool audio_active = false;
  bool current_audience = false;
  bool ptt_active = false;
#ifdef SFU_DIAG_LOG
  bool video_active_after = false;
#endif

  pthread_mutex_lock(&session->media.lock);
  audio_ssrc = answer->audio_section_present ? (answer->audio_sends ? answer->audio_ssrc : 0) : session->media.uplink_audio.ssrc;
  uint32_t rtx_ssrc = session->media.uplink_video.rtx_ssrc;
  uint32_t screen_rtx_ssrc = session->media.screen.rtx_ssrc;
  if (answer->video_section_present) {
    if (answer->video_sends) {
      rtx_ssrc = answer->rtx_ssrc;
      video_ssrc = sfu_resolve_media_ssrc(session->media.uplink_video.ssrc, session->media.uplink_video.rtx_ssrc, answer->video_ssrc, rtx_ssrc);
    } else {
      video_ssrc = 0;
      rtx_ssrc = 0;
    }
  } else {
    video_ssrc = session->media.uplink_video.ssrc;
  }
  if (answer->screen_section_present) {
    if (answer->screen_sends) {
      screen_rtx_ssrc = answer->screen_rtx_ssrc;
      screen_ssrc = sfu_resolve_media_ssrc(session->media.screen.ssrc, session->media.screen.rtx_ssrc, answer->screen_ssrc, screen_rtx_ssrc);
    } else {
      screen_ssrc = 0;
      screen_rtx_ssrc = 0;
    }
  } else {
    screen_ssrc = session->media.screen.ssrc;
  }
  uint8_t video_pt = answer->video_pt != 0 ? answer->video_pt : session->media.uplink_video.payload_type;
  uint8_t rtx_pt = answer->video_section_present ? (answer->video_sends ? answer->rtx_pt : 0) : session->media.uplink_video.rtx_payload_type;
  uint8_t screen_pt = answer->screen_pt != 0 ? answer->screen_pt : session->media.screen.payload_type;
  uint8_t screen_rtx_pt = answer->screen_section_present ? (answer->screen_sends ? answer->screen_rtx_pt : 0) : session->media.screen.rtx_payload_type;
  sfu_video_codec_t codec = answer->video_codec != SFU_VIDEO_CODEC_NONE ? (sfu_video_codec_t)answer->video_codec : session->media.uplink_video.codec;
  sfu_video_codec_t screen_codec = answer->screen_codec != SFU_VIDEO_CODEC_NONE ? (sfu_video_codec_t)answer->screen_codec : session->media.screen.codec;
  current_audience = atomic_load_explicit(&session->is_audience, memory_order_acquire);
  ptt_active = atomic_load_explicit(&session->media.ptt_active, memory_order_acquire);
  if (current_audience || (answer->video_section_present && (!answer->video_sends || video_ssrc == 0))) {
    atomic_store_explicit(&session->media.camera_rtp_observed, false, memory_order_release);
  } else if (answer->video_section_present && answer->video_sends && video_ssrc != 0) {
    atomic_store_explicit(&session->media.camera_rtp_observed, true, memory_order_release);
  }
  if (current_audience || (answer->screen_section_present && (!answer->screen_sends || screen_ssrc == 0))) {
    atomic_store_explicit(&session->media.screen_rtp_observed, false, memory_order_release);
  } else if (answer->screen_section_present && answer->screen_sends && screen_ssrc != 0) {
    atomic_store_explicit(&session->media.screen_rtp_observed, true, memory_order_release);
  }
  audio_active = audio_ssrc != 0 && (!current_audience || ptt_active);
  bool old_video_active = session->media.uplink_video.active;
  bool old_screen_active = session->media.screen.active;

  changed = session->media.uplink_audio.ssrc != audio_ssrc || session->media.uplink_audio.active != audio_active ||
            session->media.uplink_video.ssrc != video_ssrc || session->media.uplink_video.rtx_ssrc != rtx_ssrc ||
            session->media.uplink_video.payload_type != video_pt || session->media.uplink_video.rtx_payload_type != rtx_pt ||
            session->media.uplink_video.codec != codec || session->media.screen.ssrc != screen_ssrc || session->media.screen.rtx_ssrc != screen_rtx_ssrc ||
            session->media.screen.payload_type != screen_pt || session->media.screen.rtx_payload_type != screen_rtx_pt ||
            session->media.screen.codec != screen_codec;

  session->media.uplink_audio.ssrc = audio_ssrc;
  session->media.uplink_audio.active = audio_active;
  session->media.uplink_video.ssrc = video_ssrc;
  session->media.uplink_video.rtx_ssrc = rtx_ssrc;
  session->media.uplink_video.payload_type = video_pt;
  session->media.uplink_video.rtx_payload_type = rtx_pt;
  session->media.uplink_video.codec = codec;
  session->media.screen.ssrc = screen_ssrc;
  session->media.screen.rtx_ssrc = screen_rtx_ssrc;
  session->media.screen.payload_type = screen_pt;
  session->media.screen.rtx_payload_type = screen_rtx_pt;
  session->media.screen.codec = screen_codec;
  session->media.twcc_recv_extmap_id = answer->twcc_recv_extmap_id;
  session->media.twcc_send_extmap_id = answer->twcc_send_extmap_id;
  session->media.mid_recv_extmap_id = answer->mid_recv_extmap_id;
  bool activity_changed = sfu_session_recompute_video_activity_locked(session);
  changed = changed || activity_changed || old_video_active != session->media.uplink_video.active || old_screen_active != session->media.screen.active;
#ifdef SFU_DIAG_LOG
  video_active_after = session->media.uplink_video.active;
#endif
  sfu_session_publish_media(session);
  pthread_mutex_unlock(&session->media.lock);
  if (answer->peer_id != 0) {
    session->peer_id = answer->peer_id;
  }
  session->user_id = answer->user_id;
  session->fd = fd;
  atomic_store_explicit(&session->screen_codec_preference, answer->screen_codec_preference, memory_order_release);
  for (int i = 0; i < 128; i++) {
    session->media.pt_map[i] = (uint8_t)i;
  }
  atomic_store_explicit(&session->applied_answer_generation, answer->generation, memory_order_release);
  pthread_mutex_unlock(&session->answer_lock);

#ifdef SFU_DIAG_LOG
  SFU_LOG_INFO("session: apply_pending_answer peer=%u gen=%u audio_sends=%d audio_ssrc=%" PRIu32
               " audio_active=%d"
               " video_sends=%d video_ssrc=%" PRIu32 " video_active=%d screen_sends=%d screen_ssrc=%" PRIu32 " audience=%d ptt=%d audio_neg=%d video_neg=%d",
               session->peer_id, answer->generation, answer->audio_sends ? 1 : 0, audio_ssrc, audio_active ? 1 : 0, answer->video_sends ? 1 : 0, video_ssrc,
               video_active_after ? 1 : 0, answer->screen_sends ? 1 : 0, screen_ssrc, current_audience ? 1 : 0, ptt_active ? 1 : 0,
               atomic_load_explicit(&session->media.audio_send_negotiated, memory_order_acquire) ? 1 : 0,
               atomic_load_explicit(&session->media.video_send_negotiated, memory_order_acquire) ? 1 : 0);
#endif

  if (role_changed) {
    *role_changed = role_diff;
  }
  if (media_changed) {
    *media_changed = changed;
  }
  return true;
}

void sfu_session_request_keyframe_for_source(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir, sfu_media_kind_t source) {
  if (!w || !publisher) {
    SFU_LOG_WARN("[KF-DBG] sfu_session_request_keyframe called with NULL worker or publisher");
    return;
  }

  uint16_t owner_worker = sfu_session_owner_worker(publisher);
  if (owner_worker != w->worker_index) {
    SFU_LOG_DEBUG("[KF-DBG] Offloading KF request: current worker %u -> publisher worker %u (pub peer_id=%u)", w->worker_index, owner_worker,
                  publisher->peer_id);
    if (w->mesh) {
      bool queued = sfu_fanout_mesh_enqueue_keyframe_request_for_source(w->mesh, w->worker_index, owner_worker, publisher, source);
      if (!queued) {
        SFU_LOG_ERROR("[KF-DBG] FAILED to enqueue cross-worker KF request from %u to %u", w->worker_index, owner_worker);
      }
    } else {
      SFU_LOG_ERROR("[KF-DBG] Worker %u mesh is NULL! Cannot dispatch cross-worker KF request", w->worker_index);
    }
    return;
  }

  pthread_mutex_lock(&publisher->media.lock);
  uint32_t media_ssrc = source == SFU_MEDIA_SCREEN ? publisher->media.screen.ssrc : publisher->media.uplink_video.ssrc;
  pthread_mutex_unlock(&publisher->media.lock);

  SFU_LOG_DEBUG("[KF-DBG] Executing KF request on owner worker %u for pub peer_id=%u source=%u (uplink SSRC=%u)", w->worker_index, publisher->peer_id,
                (unsigned)source, media_ssrc);

  int64_t now = (int64_t)sfu_now_ms();
  int64_t *last_pli = source == SFU_MEDIA_SCREEN ? &publisher->egress.last_screen_pli_time : &publisher->egress.last_pli_time;
  if (*last_pli != 0 && now - *last_pli < SFU_SESSION_KF_THROTTLE_MS) {
    publisher->egress.diag.pli_coalesced++;
    sfu_metric_inc("congestion_pli_coalesced");
    SFU_LOG_DEBUG("worker %u: KF request for publisher %u source %u coalesced (last PLI %" PRId64 " ms ago)", w->worker_index, publisher->peer_id,
                  (unsigned)source, now - *last_pli);
    return;
  }

  *last_pli = now;

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
    rtcp_len = sfu_rtcp_build_fir(sfu_sender_ssrc, media_ssrc, &publisher->egress.fir_seq, rtcp_pkt->data, rtcp_pkt->cap);
  } else {
    rtcp_len = sfu_rtcp_build_pli(sfu_sender_ssrc, media_ssrc, rtcp_pkt->data, rtcp_pkt->cap);
  }

  if (rtcp_len > 0) {
    pthread_mutex_lock(&publisher->crypto_lock);
    bool protected = sfu_srtp_protect_rtcp(&publisher->srtp, rtcp_pkt->data, &rtcp_len, rtcp_pkt->cap);
    pthread_mutex_unlock(&publisher->crypto_lock);
    if (protected) {
      rtcp_pkt->len = (uint32_t)rtcp_len;

      int sent = sfu_ring_queue_send_zc(&w->send_ring, rtcp_pkt, (const struct sockaddr *)&publisher->cold->addr, publisher->cold->addr_len);
      if (sent != 0) {
        SFU_LOG_ERROR("Failed to enqueue PLI to send_ring for peer %u", publisher->peer_id);
      } else {
        publisher->egress.diag.pli_sent++;
        sfu_metric_inc("congestion_pli_sent");
      }
    } else {
      SFU_LOG_WARN("Failed to SRTP protect keyframe request for peer %u", publisher->peer_id);
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtcp_pkt);
}

void sfu_session_request_keyframe(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir) {
  sfu_session_request_keyframe_for_source(w, publisher, use_fir, SFU_MEDIA_VIDEO);
}

bool sfu_session_send_remb_for_source(sfu_worker_t *w, sfu_peer_session_t *publisher, sfu_media_kind_t source, uint32_t bitrate_bps) {
  if (!w || !publisher || (source != SFU_MEDIA_VIDEO && source != SFU_MEDIA_SCREEN) || bitrate_bps == 0 || publisher->state != SFU_SESSION_ESTABLISHED ||
      !sfu_session_accepts_work(publisher) || sfu_session_owner_worker(publisher) != w->worker_index) {
    sfu_metric_inc("remb_send_rejected");
    return false;
  }

  sfu_media_snapshot_t media = sfu_session_load_media(publisher);
  uint32_t media_ssrc = source == SFU_MEDIA_SCREEN ? media.screen_ssrc : media.video_ssrc;
  if (media_ssrc == 0) {
    sfu_metric_inc("remb_no_media_ssrc");
    return false;
  }

  sfu_packet_t *rtcp_pkt = sfu_packet_pool_alloc(w->pp);
  if (!rtcp_pkt) {
    return false;
  }

  int rtcp_len = sfu_rtcp_build_remb(1, bitrate_bps, &media_ssrc, 1, rtcp_pkt->data, rtcp_pkt->cap);
  bool sent = false;
  if (rtcp_len > 0) {
    pthread_mutex_lock(&publisher->crypto_lock);
    bool protected = sfu_srtp_protect_rtcp(&publisher->srtp, rtcp_pkt->data, &rtcp_len, rtcp_pkt->cap);
    pthread_mutex_unlock(&publisher->crypto_lock);
    if (protected) {
      rtcp_pkt->len = (uint32_t)rtcp_len;
      if (sfu_ring_queue_send_zc(&w->send_ring, rtcp_pkt, (const struct sockaddr *)&publisher->cold->addr, publisher->cold->addr_len) == 0) {
        sfu_metric_inc("remb_sent");
        sent = true;
      } else {
        sfu_metric_inc("remb_send_rejected");
      }
    } else {
      sfu_metric_inc("remb_protect_fail");
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtcp_pkt);
  return sent;
}

void sfu_session_write_remb_contribution(sfu_peer_session_t *subscriber, uint32_t remote_slot, uint64_t assignment_generation, uint32_t camera_bitrate_bps,
                                         uint32_t screen_bitrate_bps, uint64_t now_us) {
  if (!subscriber || remote_slot >= SFU_MAX_REMOTE_SLOTS || assignment_generation == 0) {
    return;
  }
  sfu_remb_contribution_t *contribution = &subscriber->graph.remb_contributions[remote_slot];
  atomic_fetch_add_explicit(&contribution->sequence, 1, memory_order_acq_rel);
  atomic_store_explicit(&contribution->assignment_generation, assignment_generation, memory_order_relaxed);
  atomic_store_explicit(&contribution->camera_bitrate_bps, camera_bitrate_bps, memory_order_relaxed);
  atomic_store_explicit(&contribution->screen_bitrate_bps, screen_bitrate_bps, memory_order_relaxed);
  atomic_store_explicit(&contribution->updated_at_us, now_us, memory_order_relaxed);
  atomic_fetch_add_explicit(&contribution->sequence, 1, memory_order_release);
  sfu_metric_inc("remb_contribution_written");
}

bool sfu_session_read_remb_contribution(const sfu_peer_session_t *subscriber, uint32_t remote_slot, uint64_t assignment_generation, uint64_t now_us,
                                        uint64_t max_age_us, uint32_t *camera_bitrate_bps, uint32_t *screen_bitrate_bps) {
  if (!subscriber || !camera_bitrate_bps || !screen_bitrate_bps || remote_slot >= SFU_MAX_REMOTE_SLOTS || assignment_generation == 0) {
    return false;
  }
  const sfu_remb_contribution_t *contribution = &subscriber->graph.remb_contributions[remote_slot];
  for (unsigned attempt = 0; attempt < 4; attempt++) {
    uint32_t sequence0 = atomic_load_explicit(&contribution->sequence, memory_order_acquire);
    if (sequence0 & 1u) {
      continue;
    }
    uint64_t generation = atomic_load_explicit(&contribution->assignment_generation, memory_order_relaxed);
    uint32_t camera_bitrate = atomic_load_explicit(&contribution->camera_bitrate_bps, memory_order_relaxed);
    uint32_t screen_bitrate = atomic_load_explicit(&contribution->screen_bitrate_bps, memory_order_relaxed);
    uint64_t updated_at_us = atomic_load_explicit(&contribution->updated_at_us, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    uint32_t sequence1 = atomic_load_explicit(&contribution->sequence, memory_order_relaxed);
    if (sequence0 != sequence1) {
      continue;
    }
    if (generation != assignment_generation || updated_at_us == 0 || now_us < updated_at_us) {
      return false;
    }
    if (now_us - updated_at_us > max_age_us) {
      sfu_metric_inc("remb_contribution_stale");
      return false;
    }
    *camera_bitrate_bps = camera_bitrate;
    *screen_bitrate_bps = screen_bitrate;
    return true;
  }
  return false;
}

static bool publisher_remb_due(int64_t last_time_us, uint32_t last_bps, uint32_t bitrate_bps, int64_t now_us) {
  if (last_time_us == 0 || last_bps == 0) {
    return true;
  }
  int64_t elapsed = now_us - last_time_us;
  uint32_t delta = bitrate_bps > last_bps ? bitrate_bps - last_bps : last_bps - bitrate_bps;
  bool decreased_20_percent = bitrate_bps < last_bps && (uint64_t)bitrate_bps * 5u <= (uint64_t)last_bps * 4u;
  bool changed_10_percent = (uint64_t)delta * 10u >= last_bps;
  return (decreased_20_percent && elapsed >= SFU_REMB_DECREASE_INTERVAL_US) || (changed_10_percent && elapsed >= SFU_REMB_NORMAL_INTERVAL_US) ||
         elapsed >= SFU_REMB_REFRESH_INTERVAL_US;
}

bool sfu_session_maybe_send_publisher_remb(sfu_worker_t *w, sfu_peer_session_t *publisher, int64_t now_us) {
  if (!w || !publisher || now_us <= 0 || !sfu_session_accepts_work(publisher) || sfu_session_owner_worker(publisher) != w->worker_index) {
    return false;
  }

  uint32_t targets[2] = {0, 0};
  uint32_t fresh = 0;
  uint32_t stale = 0;
  bool saw_route = false;
  sfu_fanout_bundle_t *bundle = sfu_session_fanout_acquire(publisher);
  for (unsigned pass = 0; pass < 2; pass++) {
    sfu_media_kind_t source = pass == 0 ? SFU_MEDIA_VIDEO : SFU_MEDIA_SCREEN;
    sfu_fanout_iter_t iter;
    sfu_fanout_iter_init(&iter, bundle, source);
    const sfu_fanout_route_t *route;
    while ((route = sfu_fanout_iter_next(&iter, NULL)) != NULL) {
      saw_route = true;
      uint32_t camera_bps = 0;
      uint32_t screen_bps = 0;
      if (route->subscriber && sfu_session_read_remb_contribution(route->subscriber, route->remote_slot, route->assignment_generation, (uint64_t)now_us,
                                                                  SFU_REMB_CONTRIBUTION_MAX_AGE_US, &camera_bps, &screen_bps)) {
        uint32_t contribution_bps = source == SFU_MEDIA_SCREEN ? screen_bps : camera_bps;
        if (contribution_bps > targets[pass]) {
          targets[pass] = contribution_bps;
        }
        fresh++;
      } else {
        stale++;
      }
    }
  }
  sfu_fanout_bundle_release(bundle);

  uint32_t previous_target_bps = publisher->egress.diag.remb_target_bps;
  uint32_t aggregate_target_bps = targets[0] > targets[1] ? targets[0] : targets[1];
  publisher->egress.diag.remb_fresh = fresh;
  publisher->egress.diag.remb_stale = stale;
  publisher->egress.diag.remb_target_bps = aggregate_target_bps;
  publisher->egress.diag.remb_sent = false;
  if (aggregate_target_bps != previous_target_bps) {
    sfu_metric_inc("remb_aggregate_target_changed");
  }

  if (targets[0] == 0 && targets[1] == 0) {
    sfu_metric_inc("remb_aggregate_no_fresh");
    if (saw_route) {
      sfu_metric_inc("remb_aggregate_empty");
    }
    return false;
  }

  bool sent = false;
  if (targets[0] > 0) {
    if (publisher_remb_due(publisher->egress.last_camera_remb_time_us, publisher->egress.last_camera_remb_bps, targets[0], now_us)) {
      if (sfu_session_send_remb_for_source(w, publisher, SFU_MEDIA_VIDEO, targets[0])) {
        publisher->egress.last_camera_remb_bps = targets[0];
        publisher->egress.last_camera_remb_time_us = now_us;
        sent = true;
      }
    } else {
      sfu_metric_inc("remb_aggregate_throttled");
    }
  }
  if (targets[1] > 0) {
    if (publisher_remb_due(publisher->egress.last_screen_remb_time_us, publisher->egress.last_screen_remb_bps, targets[1], now_us)) {
      if (sfu_session_send_remb_for_source(w, publisher, SFU_MEDIA_SCREEN, targets[1])) {
        publisher->egress.last_screen_remb_bps = targets[1];
        publisher->egress.last_screen_remb_time_us = now_us;
        sent = true;
      }
    } else {
      sfu_metric_inc("remb_aggregate_throttled");
    }
  }
  if (sent) {
    publisher->egress.diag.remb_sent = true;
    sfu_metric_inc("remb_aggregate_sent");
  }
  return sent;
}

#ifdef SFU_DIAG_LOG
bool sfu_session_congestion_diag_due(const sfu_peer_session_t *session, uint64_t now_us) {
  if (!session || now_us == 0 || !sfu_session_video_runtime_ready(session)) {
    return false;
  }
  sfu_media_snapshot_t media = sfu_session_load_media(session);
  if (!media.video_active && !media.screen_active && session->egress.diag.allocation_streams == 0) {
    return false;
  }
  uint64_t phase = ((uint64_t)session->peer_id * 2654435761ULL) % SFU_CONGESTION_DIAG_INTERVAL_US;
  if (session->egress.diag.last_log_us == 0) {
    return now_us % SFU_CONGESTION_DIAG_INTERVAL_US >= phase;
  }
  uint64_t current_window = now_us >= phase ? (now_us - phase) / SFU_CONGESTION_DIAG_INTERVAL_US : 0;
  uint64_t last_window = session->egress.diag.last_log_us >= phase ? (session->egress.diag.last_log_us - phase) / SFU_CONGESTION_DIAG_INTERVAL_US : UINT64_MAX;
  return current_window > last_window;
}

static size_t diag_append(char *buf, size_t cap, size_t used, const char *fmt, ...) {
  if (used >= cap) {
    return used;
  }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + used, cap - used, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return used;
  }
  size_t added = (size_t)n;
  return added >= cap - used ? cap - 1 : used + added;
}

static uint64_t diag_counter_delta(uint64_t current, uint64_t previous) { return current >= previous ? current - previous : current; }

void sfu_session_log_congestion_diag(sfu_worker_t *w, sfu_peer_session_t *session, uint64_t now_us) {
  if (!w || !session || sfu_session_owner_worker(session) != w->worker_index || !sfu_session_congestion_diag_due(session, now_us)) {
    return;
  }
  sfu_congestion_diag_t *diag = &session->egress.diag;
  char allocations[768];
  size_t allocation_used = 0;
  allocations[0] = '\0';
  uint32_t listed = 0;
  if (session->egress.schedulers) {
    for (uint32_t i = 0; i < SFU_LAYER_SCHEDULER_CAP && listed < 8; i++) {
      const sfu_layer_scheduler_slot_t *slot = &session->egress.schedulers[i];
      if (slot->publisher_id == 0 || slot->sched.allocated_bps == 0) {
        continue;
      }
      uint32_t publisher = (uint32_t)(slot->publisher_id >> 8);
      uint8_t source = (uint8_t)slot->publisher_id;
      allocation_used = diag_append(allocations, sizeof(allocations), allocation_used, "%s%u/%c=%u:t%u/%u", listed ? "," : "", publisher,
                                    source == SFU_MEDIA_SCREEN ? 's' : 'c', slot->sched.allocated_bps, slot->sched.current_tid, slot->sched.target_tid);
      listed++;
    }
  }
  bool allocations_truncated = diag->allocation_streams > listed;
  if (allocations_truncated) {
    allocation_used = diag_append(allocations, sizeof(allocations), allocation_used, "%s+%u", listed ? "," : "", diag->allocation_streams - listed);
  }
  if (allocation_used >= sizeof(allocations) - 1) {
    allocations_truncated = true;
  }

  uint64_t pacer_drops = session->egress.pacer.dropped_enh;
  uint64_t rtx_budget_drops = session->egress.pacer.rtx_dropped_budget;
  uint64_t pacer_delta = diag_counter_delta(pacer_drops, diag->last_logged_pacer_drops);
  uint64_t rtx_drop_delta = diag_counter_delta(rtx_budget_drops, diag->last_logged_rtx_budget_drops);
  uint64_t nack_delta = diag_counter_delta(diag->nack_requests, diag->last_logged_nack_requests);
  uint64_t cache_hit_delta = diag_counter_delta(diag->cache_hits, diag->last_logged_cache_hits);
  uint64_t cache_miss_delta = diag_counter_delta(diag->cache_misses, diag->last_logged_cache_misses);
  uint64_t rtx_delta = diag_counter_delta(diag->rtx_sent, diag->last_logged_rtx_sent);
  uint64_t pli_received_delta = diag_counter_delta(diag->pli_received, diag->last_logged_pli_received);
  uint64_t pli_sent_delta = diag_counter_delta(diag->pli_sent, diag->last_logged_pli_sent);
  uint64_t pli_coalesced_delta = diag_counter_delta(diag->pli_coalesced, diag->last_logged_pli_coalesced);
  int64_t debt = session->egress.pacer.balance_bytes < 0 ? -session->egress.pacer.balance_bytes : 0;
  SFU_LOG_INFO(
      "congestion session=%u worker=%u gcc=%u ack=%u overuse=%u twcc_loss=%u/%u pool=%u reserve=%u "
      "alloc=%u unalloc=%u streams=[%s] alloc_truncated=%u pacer_bps=%u debt=%" PRId64 " drop_delta=%" PRIu64 " rtx_drop_delta=%" PRIu64 " nack_delta=%" PRIu64
      " cache_delta=%" PRIu64 "/%" PRIu64 " rtx_delta=%" PRIu64 " pli_delta=%" PRIu64 "/%" PRIu64 "/%" PRIu64
      " remb=contrib:%u,target:%u,last_camera:%u,last_screen:%u,sent:%u,fresh:%u,stale:%u",
      session->peer_id, w->worker_index, diag->latest_gcc_bps, diag->latest_ack_bps, diag->latest_overuse, diag->latest_twcc_lost, diag->latest_twcc_total,
      diag->allocation_pool_bps, diag->allocation_reserve_bps, diag->allocation_allocated_bps, diag->allocation_unallocated_bps, allocations,
      allocations_truncated ? 1u : 0u, session->egress.pacer.pacing_bps, debt, pacer_delta, rtx_drop_delta, nack_delta, cache_hit_delta, cache_miss_delta,
      rtx_delta, pli_received_delta, pli_sent_delta, pli_coalesced_delta, diag->remb_contribution_bps, diag->remb_target_bps,
      session->egress.last_camera_remb_bps, session->egress.last_screen_remb_bps, diag->remb_sent ? 1u : 0u, diag->remb_fresh, diag->remb_stale);
  diag->last_logged_nack_requests = diag->nack_requests;
  diag->last_logged_cache_hits = diag->cache_hits;
  diag->last_logged_cache_misses = diag->cache_misses;
  diag->last_logged_rtx_sent = diag->rtx_sent;
  diag->last_logged_pli_received = diag->pli_received;
  diag->last_logged_pli_sent = diag->pli_sent;
  diag->last_logged_pli_coalesced = diag->pli_coalesced;
  diag->last_logged_pacer_drops = pacer_drops;
  diag->last_logged_rtx_budget_drops = rtx_budget_drops;
  diag->last_log_us = now_us;
  sfu_metric_inc("congestion_diag_log");
}
#endif

void sfu_session_maybe_send_twcc_feedback(sfu_worker_t *w, sfu_peer_session_t *publisher) {
  if (!w || !publisher || !sfu_session_video_runtime_ready(publisher) || !publisher->egress.twcc_recv) {
    return;
  }
  if (sfu_session_owner_worker(publisher) != w->worker_index) {
    return;
  }

  sfu_twcc_recv_tracker_t *t = publisher->egress.twcc_recv;
  if (!sfu_twcc_recv_tracker_pending(t)) {
    return;
  }

  int64_t now_us = (int64_t)sfu_now_us();
  if (t->last_feedback_us != 0 && now_us - t->last_feedback_us < SFU_TWCC_FEEDBACK_INTERVAL_US) {
    return;
  }

  uint32_t media_ssrc = publisher->media.uplink_video.ssrc ? publisher->media.uplink_video.ssrc : publisher->media.uplink_audio.ssrc;
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

    pthread_mutex_lock(&publisher->crypto_lock);
    bool protected = sfu_srtp_protect_rtcp(&publisher->srtp, rtcp_pkt->data, &rtcp_len, rtcp_pkt->cap);
    pthread_mutex_unlock(&publisher->crypto_lock);
    if (protected) {
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
