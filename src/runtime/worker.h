#ifndef SFU_RUNTIME_WORKER_H
#define SFU_RUNTIME_WORKER_H

#include <pthread.h>
#include <stdint.h>

#include "memory/packet_pool.h"
#include "memory/worker_packet_arena.h"
#include "net/io_uring.h"
#include "room/room_registry.h"
#include "runtime/fanout.h"
#include "runtime/routing_context.h"
#include "transport/stun/stun.h"
#include "util/ringbuffer.h"

typedef struct sfu_scheduler sfu_scheduler_t;

#define SFU_WORKER_OUTPUT_ARENA_CAPACITY 2048u

typedef struct sfu_worker_hot_counters {
  uint64_t output_reserved;
  uint64_t output_arena_allocated;
  uint64_t output_pool_fallback;
  uint64_t output_queued;
  uint64_t copied_bytes;
  uint64_t profile_samples;
  uint64_t copy_cycles;
  uint64_t crypto_cycles;
  uint32_t profile_sequence;
} sfu_worker_hot_counters_t;

typedef struct sfu_worker {
  sfu_ring_t send_ring;
  sfu_spsc_ring_t inbox;
  sfu_spsc_ring_t release_to_dispatcher;
  sfu_worker_packet_arena_t output_arena;
  sfu_packet_t *reserved_outputs[SFU_FANOUT_BATCH_CAP];
  sfu_worker_hot_counters_t hot;
  sfu_packet_pool_t *pp;
  sfu_room_registry_t *room_registry;
  sfu_fanout_mesh_t *mesh;
  sfu_session_table_t *sessions;
  sfu_routing_table_t *routing_table;
  const sfu_ice_credentials_t *ice_creds;
  sfu_scheduler_t *scheduler;
  pthread_t thread;
  _Atomic uint64_t generation;
  int64_t last_twcc_flush_us;
  int64_t last_remb_scan_us;
#ifdef SFU_DIAG_LOG
  int64_t last_diag_scan_us;
#endif
  uint32_t worker_index;
  int core_id;
  int fd;

  sfu_peer_session_t **local_sessions;
  uint32_t local_session_count;
  uint32_t local_session_capacity;
  sfu_peer_session_t **twcc_scratch;
  uint32_t twcc_scratch_capacity;
  pthread_mutex_t local_sessions_lock;
} sfu_worker_t;

int sfu_worker_init(sfu_worker_t *w, int core_id, uint32_t worker_index, int fd, sfu_packet_pool_t *pp, sfu_room_registry_t *room_registry,
                    sfu_fanout_mesh_t *mesh, sfu_session_table_t *sessions, sfu_routing_table_t *routing_table, const sfu_ice_credentials_t *ice_creds,
                    sfu_scheduler_t *scheduler, uint32_t inbox_capacity, int send_bgid);
void sfu_worker_destroy(sfu_worker_t *w);
int sfu_worker_start(sfu_worker_t *w);
void sfu_worker_join(sfu_worker_t *w);
bool sfu_worker_register_session(sfu_worker_t *w, sfu_peer_session_t *s);
void sfu_worker_unregister_session(sfu_worker_t *w, sfu_peer_session_t *s);

#endif /* SFU_RUNTIME_WORKER_H */
