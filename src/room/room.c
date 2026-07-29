#include "room/room.h"
#include <inttypes.h>
#include <string.h>
#include "util/alloc.h"

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

  room->peer_capacity = SFU_ROOM_MAX_PEERS;

  room->peers = SFU_CALLOC(room->peer_capacity, sizeof(*room->peers));

  if (!room->peers) {
    return -1;
  }

  return 0;
}

void sfu_room_destroy(sfu_room_t *room) {
  SFU_FREE(room->peers);
  room->peers = NULL;
  pthread_mutex_destroy(&room->lock);
}
