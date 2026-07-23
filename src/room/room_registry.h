#ifndef SFU_ROOM_REGISTRY_H
#define SFU_ROOM_REGISTRY_H

#include <pthread.h>
#include "sfu/datadef.h"

#define SFU_MAX_ROOMS 128

typedef struct sfu_room_registry {
  sfu_room_t rooms[SFU_MAX_ROOMS];
  uint32_t room_count;
  pthread_mutex_t lock;
} sfu_room_registry_t;

int sfu_room_registry_init(sfu_room_registry_t *reg);
void sfu_room_registry_destroy(sfu_room_registry_t *reg);

/*
 * Finds an existing room by room_id, or initializes a new one if not found.
 * Returns a pointer to the room, or NULL if the registry is full.
 */
sfu_room_t *sfu_room_registry_get_or_create(sfu_room_registry_t *reg, uint64_t room_id);

#endif /* SFU_ROOM_REGISTRY_H */
