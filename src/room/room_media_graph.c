#include "room/room_media_graph.h"
#include <pthread.h>
#include "util/alloc.h"
#include "util/log.h"

static sfu_receiver_slot_t *alloc_receiver_slot(sfu_peer_session_t *peer, sfu_scheduler_t *scheduler) {
  for (uint32_t i = 0; i < peer->receiver_capacity; i++) {
    sfu_receiver_slot_t *slot = peer->receivers[i];
    if (slot && slot->audio == NULL && slot->video == NULL) {
      return slot;
    }
  }

  uint32_t new_capacity = peer->receiver_capacity == 0 ? 4 : peer->receiver_capacity * 2;
  sfu_receiver_slot_t **new_array = SFU_CALLOC(new_capacity, sizeof(*new_array));
  if (!new_array) {
    return NULL;
  }
  for (uint32_t i = 0; i < peer->receiver_capacity; i++) {
    new_array[i] = peer->receivers[i];
  }
  sfu_receiver_slot_t *slot = SFU_CALLOC(1, sizeof(*slot));
  if (!slot) {
    SFU_FREE(new_array);
    return NULL;
  }
  new_array[peer->receiver_capacity] = slot;

  sfu_receiver_slot_t **old_array = peer->receivers;

  __atomic_store_n(&peer->receiver_capacity, new_capacity, __ATOMIC_RELEASE);
  __atomic_store_n(&peer->receivers, new_array, __ATOMIC_RELEASE);

  if (old_array) {
    if (scheduler) {
      sfu_scheduler_retire_ptr(scheduler, old_array);
    } else {
      SFU_LOG_ERROR("alloc_receiver_slot: no scheduler to retire old_array=%p, leaking", (void *)old_array);
    }
  }

  return slot;
}

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer, sfu_scheduler_t *scheduler) {
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

  room->peers[room->peer_count++] = peer;
  peer->room = room;

  for (uint32_t i = 0; i + 1 < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];

    if (!other || other == peer) {
      continue;
    }

    sfu_receiver_slot_t *slot = alloc_receiver_slot(other, scheduler);
    if (slot) {
      slot->audio = &peer->uplink_audio;
      slot->video = &peer->uplink_video;
    }

    slot = alloc_receiver_slot(peer, scheduler);
    if (slot) {
      slot->audio = &other->uplink_audio;
      slot->video = &other->uplink_video;
    }

    other->negotiation_needed = true;
    peer->negotiation_needed = true;
  }

  pthread_mutex_unlock(&room->lock);
}

void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  pthread_mutex_lock(&room->lock);

  uint32_t idx = UINT32_MAX;
  for (uint32_t i = 0; i < room->peer_count; i++) {
    if (room->peers[i] == peer) {
      idx = i;
      break;
    }
  }
  if (idx != UINT32_MAX) {
    room->peers[idx] = room->peers[room->peer_count - 1];
    room->peers[room->peer_count - 1] = NULL;
    room->peer_count--;
  }

  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || !other->receivers) {
      continue;
    }

    bool touched = false;
    for (uint32_t s = 0; s < other->receiver_capacity; s++) {
      sfu_receiver_slot_t *slot = other->receivers[s];
      if (!slot) {
        continue;
      }
      if ((slot->audio && slot->audio->owner == peer) || (slot->video && slot->video->owner == peer)) {
        slot->audio = NULL;
        slot->video = NULL;
        touched = true;
      }
    }
    if (touched) {
      other->negotiation_needed = true;
    }
  }

  if (peer->receivers) {
    for (uint32_t i = 0; i < peer->receiver_capacity; i++) {
      if (peer->receivers[i]) {
        peer->receivers[i]->audio = NULL;
        peer->receivers[i]->video = NULL;
      }
    }
  }

  peer->room = NULL;
  peer->negotiation_needed = false;

  pthread_mutex_unlock(&room->lock);
}
