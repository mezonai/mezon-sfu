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

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx);
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
void sfu_session_request_keyframe(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir);
void sfu_session_maybe_send_twcc_feedback(sfu_worker_t *w, sfu_peer_session_t *publisher);
sfu_receiver_snapshot_t *sfu_session_subscriptions_acquire(const sfu_peer_session_t *s);
void sfu_subscriptions_snapshot_release(sfu_receiver_snapshot_t *snap);
void sfu_session_publish_receivers(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap);
sfu_receiver_snapshot_t *sfu_session_fanout_targets_acquire(const sfu_peer_session_t *s);
void sfu_session_publish_fanout_targets(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap);

static inline bool sfu_session_accepts_work(const sfu_peer_session_t *s) { return atomic_load_explicit(&s->accepts_work, memory_order_acquire); }

/**
 * Load the media snapshot via seqlock (lock-free, hot path).
 * Retries if a concurrent write is detected.
 */
static inline sfu_media_snapshot_t sfu_session_load_media(const sfu_peer_session_t *s) {
  sfu_media_snapshot_t snap;
  uint64_t words[3];
  uint32_t seq0, seq1;
  do {
    seq0 = atomic_load_explicit(&s->media_snap_seq, memory_order_acquire);
    while (seq0 & 1u) {
      seq0 = atomic_load_explicit(&s->media_snap_seq, memory_order_acquire);
    }
    words[0] = atomic_load_explicit(&s->media_snap_words[0], memory_order_relaxed);
    words[1] = atomic_load_explicit(&s->media_snap_words[1], memory_order_relaxed);
    words[2] = atomic_load_explicit(&s->media_snap_words[2], memory_order_relaxed);
    seq1 = atomic_load_explicit(&s->media_snap_seq, memory_order_acquire);
  } while (seq0 != seq1);
  memcpy(&snap, words, sizeof(snap));
  return snap;
}

/**
 * Publish a new media snapshot via seqlock.
 * Must be called under media_lock to serialize concurrent writers.
 */
static inline void sfu_session_publish_media(sfu_peer_session_t *s) {
  sfu_media_snapshot_t snap = {
      .audio_ssrc = s->uplink_audio.ssrc,
      .video_ssrc = s->uplink_video.ssrc,
      .video_rtx_ssrc = s->uplink_video.rtx_ssrc,
      .video_pt = s->uplink_video.payload_type,
      .video_rtx_pt = s->uplink_video.rtx_payload_type,
      .video_codec = (uint8_t)s->uplink_video.codec,
      .twcc_recv_extmap_id = s->twcc_recv_extmap_id,
      .twcc_send_extmap_id = s->twcc_send_extmap_id,
      .audio_active = s->uplink_audio.active,
      .video_active = s->uplink_video.active,
  };
  uint64_t words[3] = {0, 0, 0};
  memcpy(words, &snap, sizeof(snap));
  atomic_fetch_add_explicit(&s->media_snap_seq, 1, memory_order_acq_rel);
  atomic_store_explicit(&s->media_snap_words[0], words[0], memory_order_relaxed);
  atomic_store_explicit(&s->media_snap_words[1], words[1], memory_order_relaxed);
  atomic_store_explicit(&s->media_snap_words[2], words[2], memory_order_relaxed);
  atomic_fetch_add_explicit(&s->media_snap_seq, 1, memory_order_release);
}

#define SFU_SESSION_OWNER_NONE UINT16_MAX

static inline uint16_t sfu_session_owner_worker(const sfu_peer_session_t *s) {
  return (uint16_t)atomic_load_explicit(&s->worker_owner, memory_order_acquire);
}

static inline uint64_t sfu_session_owner_generation(const sfu_peer_session_t *s) {
  return atomic_load_explicit(&s->worker_owner, memory_order_acquire) >> 16;
}

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
  return session->pt_map[incoming_pt & 0x7F];
}

#endif /* SFU_PEER_SESSION_H */
