#ifndef SFU_PEER_SESSION_H
#define SFU_PEER_SESSION_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "runtime/worker.h"
#include "sfu/datadef.h"

typedef struct sfu_worker sfu_worker_t;
typedef struct sfu_pending_answer sfu_pending_answer_t;

typedef enum sfu_session_rebind_result {
  SFU_SESSION_REBIND_UNCHANGED = 0,
  SFU_SESSION_REBIND_APPLIED,
  SFU_SESSION_REBIND_REJECTED,
} sfu_session_rebind_result_t;

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx, void *workers, uint32_t worker_count);
void sfu_session_table_destroy(sfu_session_table_t *t);
sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);
sfu_peer_session_t *sfu_session_table_get_or_create_by_ufrag(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len, const char *ufrag,
                                                             bool allow_rebind, sfu_session_rebind_result_t *out_rebind);
sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);
sfu_peer_session_t *sfu_session_table_find_by_ufrag(sfu_session_table_t *t, const char *ufrag);
typedef void (*sfu_session_iter_fn)(sfu_peer_session_t *s, void *user);
uint32_t sfu_session_table_foreach(sfu_session_table_t *t, sfu_session_iter_fn fn, void *user);
void sfu_session_release(sfu_peer_session_t *s);
bool sfu_session_begin_close(sfu_session_table_t *t, sfu_peer_session_t *s);
void sfu_session_table_remove(sfu_session_table_t *t, sfu_peer_session_t *s);
bool sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len);
bool sfu_session_table_index_ufrag(sfu_session_table_t *t, sfu_peer_session_t *session);
bool sfu_session_apply_pending_answer(sfu_peer_session_t *session, const sfu_pending_answer_t *answer, int fd, bool *role_changed, bool *media_changed);
bool sfu_session_ensure_video_runtime(sfu_peer_session_t *session);
bool sfu_session_recompute_video_activity_locked(sfu_peer_session_t *session);
static inline bool sfu_session_video_runtime_ready(const sfu_peer_session_t *session) {
  return atomic_load_explicit(&session->egress.video_runtime_state, memory_order_acquire) == SFU_VIDEO_RUNTIME_READY;
}
void sfu_session_request_keyframe_for_source(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir, sfu_media_kind_t source);
void sfu_session_request_keyframe(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir);
void sfu_session_maybe_send_twcc_feedback(sfu_worker_t *w, sfu_peer_session_t *publisher);
bool sfu_session_send_remb(sfu_worker_t *w, sfu_peer_session_t *publisher, uint32_t bitrate_bps);
void sfu_session_write_remb_contribution(sfu_peer_session_t *subscriber, uint32_t remote_slot, uint64_t assignment_generation,
                                         uint32_t bitrate_bps, uint64_t now_us);
bool sfu_session_read_remb_contribution(const sfu_peer_session_t *subscriber, uint32_t remote_slot, uint64_t assignment_generation,
                                        uint64_t now_us, uint64_t max_age_us, uint32_t *bitrate_bps);
bool sfu_session_maybe_send_publisher_remb(sfu_worker_t *w, sfu_peer_session_t *publisher, int64_t now_us);
#ifdef SFU_DIAG_LOG
bool sfu_session_congestion_diag_due(const sfu_peer_session_t *session, uint64_t now_us);
void sfu_session_log_congestion_diag(sfu_worker_t *w, sfu_peer_session_t *session, uint64_t now_us);
#endif
typedef struct sfu_receiver_snapshot_iter {
  const sfu_receiver_snapshot_t *snapshot;
  uint32_t chunk_index;
  uint32_t occupied;
} sfu_receiver_snapshot_iter_t;

sfu_receiver_snapshot_t *sfu_receiver_snapshot_alloc(void);
const sfu_receiver_entry_t *sfu_receiver_snapshot_at(const sfu_receiver_snapshot_t *snap, uint32_t remote_slot);
void sfu_receiver_snapshot_iter_init(sfu_receiver_snapshot_iter_t *iter, const sfu_receiver_snapshot_t *snap);
const sfu_receiver_entry_t *sfu_receiver_snapshot_iter_next(sfu_receiver_snapshot_iter_t *iter, uint32_t *remote_slot);
const sfu_receiver_entry_t *sfu_receiver_snapshot_nth(const sfu_receiver_snapshot_t *snap, uint32_t ordinal, uint32_t *remote_slot);
const sfu_receiver_entry_t *sfu_receiver_snapshot_find_peer(const sfu_receiver_snapshot_t *snap, const sfu_peer_session_t *peer, uint32_t *remote_slot);
bool sfu_receiver_snapshot_set(sfu_receiver_snapshot_t *snap, uint32_t remote_slot, const sfu_receiver_entry_t *entry);
sfu_receiver_snapshot_t *sfu_receiver_snapshot_copy_set(const sfu_receiver_snapshot_t *old, uint32_t remote_slot, const sfu_receiver_entry_t *entry);
sfu_receiver_snapshot_t *sfu_session_subscriptions_acquire(const sfu_peer_session_t *s);
void sfu_subscriptions_snapshot_release(sfu_receiver_snapshot_t *snap);
void sfu_session_publish_receivers(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap);
sfu_receiver_snapshot_t *sfu_session_publish_receivers_swap(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap);
typedef struct sfu_fanout_iter {
  const sfu_fanout_bundle_t *bundle;
  uint32_t chunk_index;
  uint32_t eligible;
  sfu_media_kind_t kind;
} sfu_fanout_iter_t;

sfu_fanout_bundle_t *sfu_fanout_bundle_alloc(void);
const sfu_fanout_route_t *sfu_fanout_bundle_at(const sfu_fanout_bundle_t *bundle, uint32_t slot);
const sfu_fanout_route_t *sfu_fanout_bundle_find_peer(const sfu_fanout_bundle_t *bundle, const sfu_peer_session_t *peer, uint32_t *slot);
bool sfu_fanout_bundle_set(sfu_fanout_bundle_t *bundle, uint32_t slot, const sfu_fanout_route_t *route, uint8_t eligibility);
sfu_fanout_bundle_t *sfu_fanout_bundle_copy_set(const sfu_fanout_bundle_t *old, uint32_t slot, const sfu_fanout_route_t *route, uint8_t eligibility);
void sfu_fanout_iter_init(sfu_fanout_iter_t *iter, const sfu_fanout_bundle_t *bundle, sfu_media_kind_t kind);
const sfu_fanout_route_t *sfu_fanout_iter_next(sfu_fanout_iter_t *iter, uint32_t *slot);
sfu_fanout_bundle_t *sfu_session_fanout_acquire(const sfu_peer_session_t *s);
void sfu_fanout_bundle_release(sfu_fanout_bundle_t *bundle);
void sfu_session_publish_fanout(sfu_peer_session_t *owner, sfu_fanout_bundle_t *bundle);
sfu_fanout_bundle_t *sfu_session_publish_fanout_swap(sfu_peer_session_t *owner, sfu_fanout_bundle_t *bundle);

void sfu_snapshot_reclaim_receivers(sfu_receiver_snapshot_t *old);
void sfu_snapshot_reclaim_fanout(sfu_fanout_bundle_t *old);

bool sfu_session_remote_slot_reserve(sfu_peer_session_t *session, int64_t publisher_user_id, uint32_t publisher_peer_id, uint32_t *slot,
                                     uint64_t *assignment_generation);
bool sfu_session_remote_slot_retire(sfu_peer_session_t *session, uint32_t slot, uint64_t assignment_generation);
sfu_remote_offer_manifest_t *sfu_session_remote_offer_capture(sfu_peer_session_t *session);
bool sfu_session_remote_offer_install(sfu_peer_session_t *session, sfu_remote_offer_manifest_t *manifest);
sfu_remote_offer_manifest_t *sfu_session_remote_offer_acquire_current(sfu_peer_session_t *session);
void sfu_remote_offer_manifest_retain(sfu_remote_offer_manifest_t *manifest);
void sfu_remote_offer_manifest_release(sfu_remote_offer_manifest_t *manifest);
bool sfu_session_remote_offer_apply_answer(sfu_peer_session_t *session, const sfu_remote_offer_manifest_t *manifest);
bool sfu_session_remote_slot_authorized(const sfu_peer_session_t *session, uint32_t slot, uint64_t assignment_generation);
bool sfu_session_remote_slots_pending(const sfu_peer_session_t *session, uint32_t *active_unapplied, uint32_t *obsolete_applied);
uint32_t sfu_session_remote_slot_high_water(const sfu_peer_session_t *session);
void sfu_session_remote_slots_teardown(sfu_peer_session_t *session);
void sfu_session_graph_assert_invariants(const sfu_peer_session_t *session);

static inline uint32_t sfu_remote_slot_first_mid(uint32_t slot) { return SFU_REMOTE_MID_BASE + slot * SFU_REMOTE_TRANSCEIVERS_PER_SLOT; }

static inline bool sfu_session_accepts_work(const sfu_peer_session_t *s) { return atomic_load_explicit(&s->accepts_work, memory_order_acquire); }

static inline sfu_media_snapshot_t sfu_session_load_media(const sfu_peer_session_t *s) {
  _Static_assert(sizeof(sfu_media_snapshot_t) <= sizeof(uint64_t) * 5, "media snapshot exceeds packed atomic storage");
  sfu_media_snapshot_t snap;
  uint64_t words[5];
  uint32_t seq0, seq1;
  do {
    seq0 = atomic_load_explicit(&s->media.snapshot_seq, memory_order_acquire);
    while (seq0 & 1u) {
      seq0 = atomic_load_explicit(&s->media.snapshot_seq, memory_order_acquire);
    }
    words[0] = atomic_load_explicit(&s->media.snapshot_words[0], memory_order_relaxed);
    words[1] = atomic_load_explicit(&s->media.snapshot_words[1], memory_order_relaxed);
    words[2] = atomic_load_explicit(&s->media.snapshot_words[2], memory_order_relaxed);
    words[3] = atomic_load_explicit(&s->media.snapshot_words[3], memory_order_relaxed);
    words[4] = atomic_load_explicit(&s->media.snapshot_words[4], memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    seq1 = atomic_load_explicit(&s->media.snapshot_seq, memory_order_acquire);
  } while (seq0 != seq1);
  memcpy(&snap, words, sizeof(snap));
  return snap;
}

static inline void sfu_session_publish_media(sfu_peer_session_t *s) {
  _Static_assert(sizeof(sfu_media_snapshot_t) <= sizeof(uint64_t) * 5, "media snapshot exceeds packed atomic storage");
  sfu_media_snapshot_t snap = {
      .audio_ssrc = s->media.uplink_audio.ssrc,
      .video_ssrc = s->media.uplink_video.ssrc,
      .video_rtx_ssrc = s->media.uplink_video.rtx_ssrc,
      .screen_ssrc = s->media.screen.ssrc,
      .screen_rtx_ssrc = s->media.screen.rtx_ssrc,
      .video_pt = s->media.uplink_video.payload_type,
      .video_rtx_pt = s->media.uplink_video.rtx_payload_type,
      .screen_pt = s->media.screen.payload_type,
      .screen_rtx_pt = s->media.screen.rtx_payload_type,
      .video_codec = (uint8_t)s->media.uplink_video.codec,
      .screen_codec = (uint8_t)s->media.screen.codec,
      .twcc_recv_extmap_id = s->media.twcc_recv_extmap_id,
      .twcc_send_extmap_id = s->media.twcc_send_extmap_id,
      .mid_recv_extmap_id = s->media.mid_recv_extmap_id,
      .audio_active = s->media.uplink_audio.active,
      .video_active = s->media.uplink_video.active,
      .screen_active = s->media.screen.active,
  };
  uint64_t words[5] = {0, 0, 0, 0, 0};
  memcpy(words, &snap, sizeof(snap));
  atomic_fetch_add_explicit(&s->media.snapshot_seq, 1, memory_order_acq_rel);
  atomic_store_explicit(&s->media.snapshot_words[0], words[0], memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[1], words[1], memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[2], words[2], memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[3], words[3], memory_order_relaxed);
  atomic_store_explicit(&s->media.snapshot_words[4], words[4], memory_order_relaxed);
  atomic_fetch_add_explicit(&s->media.snapshot_seq, 1, memory_order_release);
}

#define SFU_SESSION_OWNER_NONE UINT16_MAX

static inline uint16_t sfu_session_owner_worker(const sfu_peer_session_t *s) { return (uint16_t)atomic_load_explicit(&s->worker_owner, memory_order_acquire); }

static inline uint64_t sfu_session_owner_generation(const sfu_peer_session_t *s) { return atomic_load_explicit(&s->worker_owner, memory_order_acquire) >> 16; }

static inline uint64_t sfu_session_set_owner_worker(sfu_peer_session_t *s, uint16_t worker_id) {
  uint64_t old = atomic_load_explicit(&s->worker_owner, memory_order_relaxed);
  for (;;) {
    uint64_t generation = (old >> 16) + 1;
    uint64_t next = (generation << 16) | worker_id;
    if (atomic_compare_exchange_weak_explicit(&s->worker_owner, &old, next, memory_order_acq_rel, memory_order_relaxed)) {
      return next;
    }
  }
}

static inline uint8_t sfu_session_get_mapped_pt(const sfu_peer_session_t *session, uint8_t incoming_pt) {
  /* Mask to 7 bits just in case, ensuring we never cause an out-of-bounds read */
  return session->media.pt_map[incoming_pt & 0x7F];
}

#endif /* SFU_PEER_SESSION_H */
