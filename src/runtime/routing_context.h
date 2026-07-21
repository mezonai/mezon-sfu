#ifndef SFU_ROUTING_CONTEXT_H
#define SFU_ROUTING_CONTEXT_H

#include <pthread.h>
#include <stdbool.h>
#include "room/room.h"

#define SFU_MAX_UFRAG_MAPPINGS 2048

typedef struct {
  char ufrag[32];
  sfu_room_t *room;
  uint32_t worker_index;
  bool has_owner;
} sfu_routing_entry_t;

typedef struct {
  sfu_routing_entry_t entries[SFU_MAX_UFRAG_MAPPINGS];
  int count;
  pthread_mutex_t mutex;
} sfu_routing_table_t;

void sfu_routing_table_init(sfu_routing_table_t *table);

#endif
