#ifndef SFU_PEER_SESSION_H
#define SFU_PEER_SESSION_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "media/transceiver.h"
#include "room/room.h"
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"

typedef enum {
  SFU_SESSION_NEW = 0,
  SFU_SESSION_DTLS_HANDSHAKING,
  SFU_SESSION_ESTABLISHED,
  SFU_SESSION_FAILED,
} sfu_session_state_t;

typedef struct sfu_receiver_slot {
  sfu_transceiver_t *video;
  sfu_transceiver_t *audio;
  sfu_peer_session_t *session;
} sfu_receiver_slot_t;

typedef struct sfu_peer_session {
  struct sockaddr_storage addr;
  socklen_t addr_len;

  uint16_t worker_id;

  sfu_session_state_t state;
  sfu_dtls_conn_t dtls;
  sfu_srtp_ctx_t srtp;
  sfu_room_t *room;
  bool active;
  char ufrag[32];
  uint8_t pt_map[128];

  sfu_transceiver_t uplink_audio;
  sfu_transceiver_t uplink_video;
  sfu_transceiver_t screen;

  sfu_receiver_slot_t receivers[SFU_MAX_REMOTE_SLOTS];

  bool negotiation_needed;
} sfu_peer_session_t;

#define SFU_SESSION_TABLE_MAX 256

typedef struct sfu_session_table {
  sfu_peer_session_t sessions[SFU_SESSION_TABLE_MAX];
  uint32_t count;
  pthread_mutex_t lock;
  sfu_dtls_ctx_t *dtls_ctx; /* shared, not owned */
} sfu_session_table_t;

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
