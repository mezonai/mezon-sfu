#ifndef SFU_ROOM_MEDIA_GRAPH_H
#define SFU_ROOM_MEDIA_GRAPH_H

#include "runtime/scheduler.h"
#include "sfu/datadef.h"

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer);
void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer);

/* Change a joined peer's self-service role. Returns false when the peer is not
 * a current room member or the requested role is already active. */
bool room_update_peer_role(sfu_room_t *room, sfu_peer_session_t *peer, bool is_audience);

#endif  // SFU_ROOM_MEDIA_GRAPH_H
