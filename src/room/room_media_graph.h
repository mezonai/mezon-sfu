#ifndef SFU_ROOM_MEDIA_GRAPH_H
#define SFU_ROOM_MEDIA_GRAPH_H

#include "room/room.h"

typedef struct sfu_room_graph_delta {
  sfu_peer_session_t *changed[SFU_ROOM_MAX_PEERS];
  uint32_t changed_count;
} sfu_room_graph_delta_t;

int sfu_room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer, sfu_room_graph_delta_t *delta);

void sfu_room_add_subscription();

void sfu_room_remove_subscription();

void sfu_room_allocate_receiver();

#endif  // SFU_ROOM_MEDIA_GRAPH_H
