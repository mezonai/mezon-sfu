#ifndef SFU_PEER_SESSION_H
#define SFU_PEER_SESSION_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "runtime/worker.h"
#include "sfu/datadef.h"

typedef struct sfu_worker sfu_worker_t;

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx);
void sfu_session_table_destroy(sfu_session_table_t *t);
sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);
sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);
sfu_peer_session_t *sfu_session_table_find_by_ufrag(sfu_session_table_t *t, const char *ufrag);
void sfu_session_table_remove(sfu_session_table_t *t, sfu_peer_session_t *s);
void sfu_session_table_rebind_addr(sfu_session_table_t *t, sfu_peer_session_t *s, const struct sockaddr_storage *addr, socklen_t addr_len);
void sfu_session_table_index_ufrag(sfu_session_table_t *t, sfu_peer_session_t *session);
void sfu_session_request_keyframe(sfu_worker_t *w, sfu_peer_session_t *publisher, bool use_fir);

static inline uint8_t sfu_session_get_mapped_pt(const sfu_peer_session_t *session, uint8_t incoming_pt) {
  /* Mask to 7 bits just in case, ensuring we never cause an out-of-bounds read */
  return session->pt_map[incoming_pt & 0x7F];
}

#endif /* SFU_PEER_SESSION_H */
