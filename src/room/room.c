#include "room/room.h"
#include <string.h>

int sfu_room_init(sfu_room_t *room, uint64_t room_id) {
  memset(room, 0, sizeof(*room));

  room->room_id = room_id;
  room->peer_capacity = SFU_ROOM_MAX_PEERS;
  room->free_count = SFU_ROOM_MAX_PEERS;
  for (uint32_t i = 0; i < SFU_ROOM_MAX_PEERS; i++) {
    room->free_slots[i] = (uint16_t)(SFU_ROOM_MAX_PEERS - 1 - i);
  }

  if (pthread_mutex_init(&room->lock, NULL) != 0) {
    return -1;
  }

  return 0;
}

void sfu_room_destroy(sfu_room_t *room) { pthread_mutex_destroy(&room->lock); }
