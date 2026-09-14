
#include "room/room_registry.h"
#include <inttypes.h>
#include <string.h>
#include "room/room.h"
#include "util/log.h"

static uint32_t room_index_bucket(uint64_t room_id) {
  room_id ^= room_id >> 30;
  room_id *= UINT64_C(0xbf58476d1ce4e5b9);
  room_id ^= room_id >> 27;
  room_id *= UINT64_C(0x94d049bb133111eb);
  room_id ^= room_id >> 31;
  return (uint32_t)room_id & (SFU_ROOM_REGISTRY_INDEX_SIZE - 1);
}

static uint32_t *room_index_entry(sfu_room_registry_t *reg, uint64_t room_id) {
  uint32_t bucket = room_index_bucket(room_id);

  for (uint32_t probe = 0; probe < SFU_ROOM_REGISTRY_INDEX_SIZE; probe++) {
    uint32_t *entry = &reg->room_index[bucket];
    if (*entry == 0 || reg->rooms[*entry - 1].room_id == room_id) {
      return entry;
    }
    bucket = (bucket + 1) & (SFU_ROOM_REGISTRY_INDEX_SIZE - 1);
  }

  return NULL;
}

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

  uint32_t *entry = room_index_entry(reg, room_id);
  if (entry != NULL && *entry != 0) {
    sfu_room_t *room = &reg->rooms[*entry - 1];
    SFU_LOG_INFO("Reusing room: ID=%" PRIu64 " room=%p peers=%u", room_id, (void *)room, room->peer_count);
    pthread_mutex_unlock(&reg->lock);
    return room;
  }

  if (reg->room_count >= SFU_MAX_ROOMS || entry == NULL) {
    SFU_LOG_ERROR("Room registry full! Cannot create room %" PRIu64, room_id);
    pthread_mutex_unlock(&reg->lock);
    return NULL;
  }

  uint32_t room_slot = reg->room_count;
  sfu_room_t *room = &reg->rooms[room_slot];

  if (sfu_room_init(room, room_id) != 0) {
    SFU_LOG_ERROR("Failed to initialize room struct for %" PRIu64, room_id);
    pthread_mutex_unlock(&reg->lock);
    return NULL;
  }

  reg->room_count++;
  *entry = room_slot + 1;
  SFU_LOG_INFO("Created new room: ID=%" PRIu64 " room=%p", room_id, (void *)room);
  pthread_mutex_unlock(&reg->lock);
  return room;
}
