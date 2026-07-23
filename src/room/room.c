#include "room/room.h"

#include <inttypes.h>
#include <string.h>

#include "media/graph.h"
#include "util/log.h"

int sfu_room_init(sfu_room_t *room, uint64_t room_id, const char *room_name) {
  memset(room, 0, sizeof(*room));

  room->room_id = room_id;

  if (room_name) {
    strncpy(room->room_name, room_name, sizeof(room->room_name) - 1);
    room->room_name[sizeof(room->room_name) - 1] = '\0';
  }

  if (pthread_mutex_init(&room->lock, NULL) != 0) {
    return -1;
  }

  sfu_media_graph_init(&room->graph);

  return 0;
}

void sfu_room_destroy(sfu_room_t *room) { pthread_mutex_destroy(&room->lock); }

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

  /*
   * Connect media graph.
   *
   * Existing peer  <---- receives ---- new peer
   * New peer       <---- receives ---- existing peer
   */
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
