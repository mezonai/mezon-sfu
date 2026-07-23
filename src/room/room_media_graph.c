#include "room/room_media_graph.h"
#include <pthread.h>
#include <string.h>
#include "util/log.h"

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

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  pthread_mutex_lock(&room->lock);

  if (peer->room == room) {
    pthread_mutex_unlock(&room->lock);
    return;
  }

  if (room->peer_count >= SFU_ROOM_MAX_PEERS) {
    pthread_mutex_unlock(&room->lock);
    SFU_LOG_WARN("room %" PRIu64 " full", room->room_id);
    return;
  }

  /* add to room */
  room->peers[room->peer_count++] = peer;
  peer->room = room;

  for (uint32_t i = 0; i + 1 < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];

    if (!other || other == peer) {
      continue;
    }

    /* other subscribes to peer */
    sfu_receiver_slot_t *slot = alloc_receiver_slot(other);
    if (slot) {
      slot->audio = &peer->uplink_audio;
      slot->video = &peer->uplink_video;
    }

    /* peer subscribes to other */
    slot = alloc_receiver_slot(peer);
    if (slot) {
      slot->audio = &other->uplink_audio;
      slot->video = &other->uplink_video;
    }

    /* both peers require renegotiation */
    other->negotiation_needed = true;
    peer->negotiation_needed = true;
  }

  pthread_mutex_unlock(&room->lock);
}

void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  pthread_mutex_lock(&room->lock);

  // todo: implement

  peer->room = NULL;
  peer->negotiation_needed = false;

  pthread_mutex_unlock(&room->lock);
}
