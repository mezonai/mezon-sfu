#ifndef SFU_PEER_SESSION_H
#define SFU_PEER_SESSION_H

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"

/*
 * One entry per peer address, tracking connection establishment state:
 * has it passed an authenticated STUN Binding Request, and has DTLS
 * finished handshaking (at which point SRTP keying material exists).
 * This is separate from room/room.h's subscriber list -- session state
 * answers "is this peer's transport ready", room membership answers
 * "who should receive this peer's media".
 *
 * KNOWN LIMITATION: same shape as room.h's registry -- fixed-capacity,
 * mutex-guarded, no timeout/eviction. Every worker touches this table
 * once per STUN/DTLS packet (not per RTP/RTCP packet, since those skip
 * straight to the room-forward path once a session is established), so
 * the lock contention is much lighter than room.h's per-packet lookup,
 * but the same "replace with a proper structure before real load"
 * caveat applies.
 */
typedef enum {
    SFU_SESSION_NEW = 0,
    SFU_SESSION_DTLS_HANDSHAKING,
    SFU_SESSION_ESTABLISHED,
    SFU_SESSION_FAILED,
} sfu_session_state_t;

typedef struct sfu_peer_session {
    struct sockaddr_storage addr;
    socklen_t                 addr_len;
    bool                        active;
    sfu_session_state_t          state;
    sfu_dtls_conn_t                dtls;
    sfu_srtp_ctx_t                  srtp; /* valid only once state == SFU_SESSION_ESTABLISHED */
} sfu_peer_session_t;

#define SFU_SESSION_TABLE_MAX 256

typedef struct sfu_session_table {
    sfu_peer_session_t sessions[SFU_SESSION_TABLE_MAX];
    uint32_t              count;
    pthread_mutex_t         lock;
    sfu_dtls_ctx_t           *dtls_ctx; /* shared, not owned */
} sfu_session_table_t;

int  sfu_session_table_init(sfu_session_table_t *t, sfu_dtls_ctx_t *dtls_ctx);
void sfu_session_table_destroy(sfu_session_table_t *t);

/* Finds the session for this sender address, creating (and DTLS-
 * initializing) one on first sight if it doesn't exist yet. Returns
 * NULL only if the table is full. The returned pointer is valid for
 * the table's lifetime (no eviction yet -- see KNOWN LIMITATION). */
sfu_peer_session_t *sfu_session_table_get_or_create(sfu_session_table_t *t,
                                                     const struct sockaddr_storage *addr,
                                                     socklen_t addr_len);

/* Read-only lookup: returns NULL if no session exists for this address
 * yet, rather than creating one. Used by the RTP/RTCP forward path,
 * which must never conjure a session (and therefore SRTP keys) out of
 * thin air for a peer that hasn't completed a DTLS handshake -- only
 * STUN/DTLS handling is allowed to create sessions. */
sfu_peer_session_t *sfu_session_table_find(sfu_session_table_t *t,
                                            const struct sockaddr_storage *addr,
                                            socklen_t addr_len);

#endif /* SFU_PEER_SESSION_H */
