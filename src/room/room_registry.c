
#include "room/room_registry.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "room/room.h"
#include "util/log.h"

int sfu_room_registry_init(sfu_room_registry_t *reg) {
  memset(reg, 0, sizeof(*reg));
  if (pthread_mutex_init(&reg->lock, NULL) != 0) {
    return -1;
  }
  return 0;
}

void sfu_room_registry_destroy(sfu_room_registry_t *reg) {
  pthread_mutex_lock(&reg->lock);
  for (uint32_t i = 0; i < reg->room_count; i++) {
    sfu_room_destroy(&reg->rooms[i]);
  }
  pthread_mutex_unlock(&reg->lock);
  pthread_mutex_destroy(&reg->lock);
}

sfu_room_t *sfu_room_registry_get_or_create(sfu_room_registry_t *reg, uint64_t room_id) {
  pthread_mutex_lock(&reg->lock);

  for (uint32_t i = 0; i < reg->room_count; i++) {
    if (reg->rooms[i].room_id == room_id) {
      SFU_LOG_INFO("Reusing room: ID=%" PRIu64 " room=%p peers=%u", room_id, (void *)&reg->rooms[i], reg->rooms[i].peer_count);
      pthread_mutex_unlock(&reg->lock);
      return &reg->rooms[i];
    }
  }

  if (reg->room_count >= SFU_MAX_ROOMS) {
    SFU_LOG_ERROR("Room registry full! Cannot create room %" PRIu64, room_id);
    pthread_mutex_unlock(&reg->lock);
    return NULL;
  }

  sfu_room_t *room = &reg->rooms[reg->room_count++];

  if (sfu_room_init(room, room_id) != 0) {
    SFU_LOG_ERROR("Failed to initialize room struct for %" PRIu64, room_id);
    reg->room_count--;
    pthread_mutex_unlock(&reg->lock);
    return NULL;
  }

  SFU_LOG_INFO("Created new room: ID=%" PRIu64 " room=%p", room_id, (void *)room);
  pthread_mutex_unlock(&reg->lock);
  return room;
}
