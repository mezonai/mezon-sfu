#include "room/room_media_graph.h"
#include <string.h>
#include "peer/session.h"

static sfu_receiver_slot_t *alloc_receiver_slot(sfu_peer_session_t *peer) {
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    sfu_receiver_slot_t *slot = &peer->receivers[i];

    if (slot->audio == NULL && slot->video == NULL) {
      memset(slot, 0, sizeof(*slot));
      return slot;
    }
  }

  return NULL;
}

int sfu_room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer, sfu_room_graph_delta_t *delta) {
  pthread_mutex_lock(&room->lock);

  if (room->peer_count >= SFU_ROOM_MAX_PEERS) {
    pthread_mutex_unlock(&room->lock);
    return -1;
  }

  room->peers[room->peer_count++] = peer;
  peer->room = room;

  for (uint32_t i = 0; i < room->peer_count - 1; i++) {
    sfu_peer_session_t *publisher = room->peers[i];

    sfu_receiver_slot_t *slot = alloc_receiver_slot(peer);

    if (!slot) {
      continue;
    }

    slot->session = peer;
    slot->audio = &publisher->uplink_audio;
    slot->video = &publisher->uplink_video;
    publisher->negotiation_needed = true;
  }

  for (uint32_t i = 0; i < room->peer_count - 1; i++) {
    sfu_peer_session_t *subscriber = room->peers[i];

    sfu_receiver_slot_t *slot = alloc_receiver_slot(subscriber);

    if (!slot) {
      continue;
    }

    slot->session = peer;
    slot->audio = &peer->uplink_audio;
    slot->video = &peer->uplink_video;

    subscriber->negotiation_needed = true;
  }

  pthread_mutex_unlock(&room->lock);

  return 0;
}
