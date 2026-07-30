#ifndef SFU_ROOM_MEDIA_GRAPH_H
#define SFU_ROOM_MEDIA_GRAPH_H

#include "runtime/scheduler.h"
#include "sfu/datadef.h"

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer, sfu_scheduler_t *scheduler);
void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer);

#endif  // SFU_ROOM_MEDIA_GRAPH_H
