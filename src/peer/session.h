#ifndef SFU_PEER_SESSION_H
#define SFU_PEER_SESSION_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "runtime/worker.h"
#include "sfu/datadef.h"

typedef struct sfu_worker sfu_worker_t;
typedef struct sfu_pending_answer sfu_pending_answer_t;

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx);
void sfu_session_table_destroy(sfu_session_table_t *t);
sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);
sfu_peer_session_t *sfu_session_table_get_or_create_by_ufrag(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len, const char *ufrag,
                                                             bool allow_rebind);
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

static inline uint8_t sfu_session_get_mapped_pt(const sfu_peer_session_t *session, uint8_t incoming_pt) {
  /* Mask to 7 bits just in case, ensuring we never cause an out-of-bounds read */
  return session->pt_map[incoming_pt & 0x7F];
}

#endif /* SFU_PEER_SESSION_H */
