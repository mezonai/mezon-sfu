#ifndef SFU_PEER_SESSION_H
#define SFU_PEER_SESSION_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "runtime/worker.h"
#include "sfu/datadef.h"

typedef struct sfu_worker sfu_worker_t;

/* ---------------------------------------------------------------------------
 * Session lifetime model (Phase 3 — fixes F-01/F-03/F-04)
 *
 * Every sfu_peer_session_t is refcounted:
 *   - +1 held by the session table from publish until sfu_session_begin_close.
 *   - +1 per caller pin returned by get_or_create / find / find_by_ufrag.
 *   - +1 per subscriber entry held by a receiver snapshot (see below).
 *
 * Ownership matrix:
 *   table slot/hash entry  -> 1 ref, dropped in begin_close under table lock
 *   lookup/create caller   -> 1 ref, must sfu_session_release() exactly once
 *   snapshot entry         -> 1 ref on the *subscriber*, released with snapshot
 *   signaling c->session   -> no ref; never dereferenced (borrowed use removed)
 *
 * A session pointer obtained from any lookup/create stays valid (allocation
 * and initialized DTLS/SRTP/RTX state included) until the caller releases it,
 * even if the session is concurrently closed via sfu_session_begin_close.
 * Closing only stops NEW references and removes the session from the table,
 * hashes, and room.
 * ------------------------------------------------------------------------- */

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx);
/* Requires that no concurrent table users exist (shutdown quiescence): all
 * workers joined, signaling stopped, and every caller pin released. Sessions
 * still OPEN are force-closed (detached from their rooms, table ref dropped),
 * then table storage is freed. */
void sfu_session_table_destroy(sfu_session_table_t *t);

/* All lookup/create functions return a caller pin (+1 ref); the caller MUST
 * sfu_session_release() it exactly once. NULL means not found / not created. */
sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);
sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);
sfu_peer_session_t *sfu_session_table_find_by_ufrag(sfu_session_table_t *t, const char *ufrag);

/* Drops one reference. When the last ref goes away the session's centralized
 * teardown runs and the allocation is freed. `s` must not be used after. */
void sfu_session_release(sfu_peer_session_t *s);

/* Acquire load of the work-acceptance flag. False means the session is
 * closing/closed and must not be given new work. */
static inline bool sfu_session_accepts_work(const sfu_peer_session_t *s) { return atomic_load_explicit(&s->accepts_work, memory_order_acquire); }

/* Idempotent logical close. The first OPEN->CLOSING transition: flips
 * accepts_work (release), removes every addr/ufrag hash entry and the table
 * slot, unlocks, detaches the room exactly once, and drops the table ref.
 * Returns true for the first (effective) close, false for repeats. The
 * session allocation and its initialized transport state stay alive until
 * the remaining refs are released. */
bool sfu_session_begin_close(sfu_session_table_t *t, sfu_peer_session_t *s);

/* Delegates to sfu_session_begin_close. */
void sfu_session_table_remove(sfu_session_table_t *t, sfu_peer_session_t *s);

/* Rebinds the transport address. Returns false (no-op) unless the session is
 * still a live OPEN table member. Old hash entries are removed by member
 * index before the new address is indexed. */
bool sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len);

/* Publishes the session's ufrag into the ufrag hash. Returns false (no-op)
 * unless the session is still a live OPEN table member. */
bool sfu_session_table_index_ufrag(sfu_session_table_t *t, sfu_peer_session_t *session);

void sfu_session_request_keyframe(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir);

/* Receiver snapshot access (F-03/F-04). sfu_session_receivers_acquire returns
 * a retained immutable snapshot (or NULL); traverse ->entries[0..count) freely
 * — each entry's subscriber is itself retained by the snapshot — then call
 * sfu_receiver_snapshot_release exactly once. */
sfu_receiver_snapshot_t *sfu_session_receivers_acquire(const sfu_peer_session_t *s);
void sfu_receiver_snapshot_release(sfu_receiver_snapshot_t *snap);
/* Writer-side publish (room lock must be held); declared here so both
 * room_media_graph.c and tests can use it. Takes ownership of new_snap. */
void sfu_session_publish_receivers(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap);

static inline uint8_t sfu_session_get_mapped_pt(const sfu_peer_session_t *session, uint8_t incoming_pt) {
  /* Mask to 7 bits just in case, ensuring we never cause an out-of-bounds read */
  return session->pt_map[incoming_pt & 0x7F];
}

#endif /* SFU_PEER_SESSION_H */
