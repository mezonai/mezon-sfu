#ifndef SFU_ROOM_MEDIA_GRAPH_H
#define SFU_ROOM_MEDIA_GRAPH_H

#include "runtime/scheduler.h"
#include "sfu/datadef.h"

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer);
void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer);
bool room_update_peer_role(sfu_room_t *room, sfu_peer_session_t *peer, bool is_audience);
void room_refresh_peer_streams(sfu_room_t *room, sfu_peer_session_t *updated_peer);
bool room_set_peer_ptt_active(sfu_room_t *room, sfu_peer_session_t *peer, bool active);

#endif  // SFU_ROOM_MEDIA_GRAPH_H
