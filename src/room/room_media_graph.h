#ifndef SFU_ROOM_MEDIA_GRAPH_H
#define SFU_ROOM_MEDIA_GRAPH_H

#include "sfu/datadef.h"

typedef enum sfu_room_admission_result {
  SFU_ROOM_ADMISSION_ERROR = 0,
  SFU_ROOM_ADMISSION_OK = 1,
  SFU_ROOM_ADMISSION_CAPACITY = 2,
} sfu_room_admission_result_t;

sfu_room_admission_result_t room_add_peer_result(sfu_room_t *room, sfu_peer_session_t *peer);
static inline bool room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  return room_add_peer_result(room, peer) == SFU_ROOM_ADMISSION_OK;
}
/* Caller must hold peer->membership_lock. */
void room_remove_peer_membership_locked(sfu_room_t *room, sfu_peer_session_t *peer);
void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer);
bool room_update_peer_role(sfu_room_t *room, sfu_peer_session_t *peer, bool is_audience);
void room_refresh_peer_streams(sfu_room_t *room, sfu_peer_session_t *updated_peer);
bool room_set_peer_ptt_active(sfu_room_t *room, sfu_peer_session_t *peer, bool active);

#endif  // SFU_ROOM_MEDIA_GRAPH_H
