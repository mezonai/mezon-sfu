#ifndef SFU_ROUTING_CONTEXT_H
#define SFU_ROUTING_CONTEXT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "sfu/datadef.h"

#define SFU_MAX_UFRAG_MAPPINGS SFU_SESSION_TABLE_MAX
#define SFU_ROUTING_UFRAG_HASH_SLOTS SFU_SESSION_UFRAG_HASH_SLOTS

_Static_assert(SFU_ROUTING_UFRAG_HASH_SLOTS >= SFU_MAX_UFRAG_MAPPINGS * 2, "routing ufrag hash table must keep load factor at or below 0.5");
_Static_assert((SFU_ROUTING_UFRAG_HASH_SLOTS & (SFU_ROUTING_UFRAG_HASH_SLOTS - 1)) == 0, "routing ufrag hash table size must be a power of two");

typedef struct sfu_pending_answer {
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t rtx_ssrc;
  uint32_t screen_ssrc;
  uint32_t screen_rtx_ssrc;
  uint32_t peer_id;
  int64_t user_id;
  uint32_t generation;
  uint8_t video_pt;
  uint8_t rtx_pt;
  uint8_t video_codec;
  uint8_t screen_pt;
  uint8_t screen_rtx_pt;
  uint8_t screen_codec;
  uint8_t screen_codec_preference;
  uint8_t twcc_recv_extmap_id;
  uint8_t twcc_send_extmap_id;
  uint8_t mid_recv_extmap_id;
  bool audio_section_present;
  bool video_section_present;
  bool screen_section_present;
  bool audio_sends;
  bool video_sends;
  bool screen_sends;
  bool is_audience;
  bool valid;
} sfu_pending_answer_t;

typedef struct sfu_routing_entry {
  char ufrag[32];
  sfu_room_t *room;
  uint32_t worker_index;
  int fd;
  bool has_owner;
  sfu_pending_answer_t pending_answer;
} sfu_routing_entry_t;

typedef struct sfu_routing_snapshot {
  sfu_room_t *room;
  uint32_t worker_index;
  int fd;
  bool has_owner;
  uint32_t pending_generation;
} sfu_routing_snapshot_t;

typedef enum sfu_routing_register_result {
  SFU_ROUTING_REGISTER_OK = 0,
  SFU_ROUTING_REGISTER_INVALID_ARGUMENT,
  SFU_ROUTING_REGISTER_OWNERSHIP_CONFLICT,
  SFU_ROUTING_REGISTER_TABLE_FULL,
} sfu_routing_register_result_t;

typedef struct {
  sfu_routing_entry_t entries[SFU_MAX_UFRAG_MAPPINGS];
  sfu_hash_slot_t ufrag_index[SFU_ROUTING_UFRAG_HASH_SLOTS];
  uint32_t count;
  uint32_t deleted_slots;
  pthread_mutex_t mutex;
} sfu_routing_table_t;

typedef struct sfu_routing_answer_reservation {
  sfu_routing_table_t *table;
  sfu_routing_entry_t *entry;
  sfu_pending_answer_t answer;
  uint32_t generation;
  uint32_t entry_index;
  size_t ufrag_len;
  char ufrag[32];
  sfu_room_t *room;
  int fd;
  bool new_entry;
  bool active;
} sfu_routing_answer_reservation_t;

int sfu_routing_table_init(sfu_routing_table_t *table);
void sfu_routing_table_destroy(sfu_routing_table_t *table);
void sfu_routing_table_unregister_fd(sfu_routing_table_t *table, int fd);

sfu_routing_register_result_t sfu_routing_table_register_answer(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd,
                                                                const sfu_pending_answer_t *answer, uint32_t *out_generation);
sfu_routing_register_result_t sfu_routing_table_prepare_answer(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd,
                                                               const sfu_pending_answer_t *answer, sfu_routing_answer_reservation_t *reservation);
bool sfu_routing_table_commit_answer(sfu_routing_answer_reservation_t *reservation, uint32_t *out_generation);
void sfu_routing_table_cancel_answer(sfu_routing_answer_reservation_t *reservation);
bool sfu_routing_table_lookup_route(sfu_routing_table_t *table, const char *client_ufrag, uint32_t worker_index, sfu_routing_snapshot_t *out);
bool sfu_routing_table_peek_route(sfu_routing_table_t *table, const char *client_ufrag, sfu_routing_snapshot_t *out);
bool sfu_routing_table_reconcile_answer(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd, uint32_t generation,
                                        sfu_peer_session_t *session, bool *role_changed, bool *media_changed);
bool sfu_routing_table_invalidate_pending(sfu_routing_table_t *table, const char *client_ufrag, sfu_room_t *room, int fd, uint32_t *out_generation);

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
