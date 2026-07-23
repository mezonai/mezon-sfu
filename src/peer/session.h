#ifndef SFU_PEER_SESSION_H
#define SFU_PEER_SESSION_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "sfu/datadef.h"
#include "transport/dtls/dtls.h"

int sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx);
void sfu_session_table_destroy(sfu_session_table_t *t);

/* Finds the session for this sender address, creating (and DTLS-
 * initializing) one on first sight if it doesn't exist yet. Returns
 * NULL only if the table is full. The returned pointer is valid for
 * the table's lifetime (no eviction yet -- see KNOWN LIMITATION). */
sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);

/* Read-only lookup: returns NULL if no session exists for this address
 * yet, rather than creating one. Used by the RTP/RTCP forward path,
 * which must never conjure a session (and therefore SRTP keys) out of
 * thin air for a peer that hasn't completed a DTLS handshake -- only
 * STUN/DTLS handling is allowed to create sessions. */
sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t, const struct sockaddr_storage *addr, socklen_t addr_len);

/**
 * Fast O(1) translation of an incoming RTP payload type to the subscriber's
 * negotiated payload type.
 */
static inline uint8_t sfu_session_get_mapped_pt(const sfu_peer_session_t *session, uint8_t incoming_pt) {
  /* Mask to 7 bits just in case, ensuring we never cause an out-of-bounds read */
  return session->pt_map[incoming_pt & 0x7F];
}

#endif /* SFU_PEER_SESSION_H */
