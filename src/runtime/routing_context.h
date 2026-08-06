#ifndef SFU_ROUTING_CONTEXT_H
#define SFU_ROUTING_CONTEXT_H

#include <pthread.h>
#include <stdbool.h>
#include "sfu/datadef.h"

#define SFU_MAX_UFRAG_MAPPINGS 2048

typedef struct {
  char ufrag[32];
  sfu_room_t *room;
  uint32_t worker_index;
  uint32_t pending_audio_ssrc;
  uint32_t pending_video_ssrc;
  uint32_t pending_rtx_ssrc;
  uint32_t peer_id;
  int fd;
  uint8_t pending_video_pt;
  uint8_t pending_rtx_pt;
  bool has_owner;
  bool has_pending_answer;
  bool is_audience;
} sfu_routing_entry_t;

typedef struct {
  sfu_routing_entry_t entries[SFU_MAX_UFRAG_MAPPINGS];
  int count;
  pthread_mutex_t mutex;
} sfu_routing_table_t;

int sfu_routing_table_init(sfu_routing_table_t *table);
void sfu_routing_table_destroy(sfu_routing_table_t *table);
void sfu_routing_table_unregister_fd(sfu_routing_table_t *table, int fd);
void sfu_routing_table_set_pending_answer(sfu_routing_table_t *table, const char *client_ufrag, uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc,
                                          uint8_t video_pt, uint8_t rtx_pt, uint32_t peer_id, bool is_audience);

static inline uint32_t fnv1a(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

#endif
