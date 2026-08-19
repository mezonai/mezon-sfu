#ifndef SFU_RUNTIME_SCHEDULER_H
#define SFU_RUNTIME_SCHEDULER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "runtime/routing_context.h"
#include "transport/stun/stun.h"

#define SFU_AFFINITY_CACHE_CAP 16384

typedef struct sfu_affinity_entry {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  uint32_t hash;
  uint32_t worker_index;
  uint64_t last_seen_ns;
  bool valid;
} sfu_affinity_entry_t;

typedef struct sfu_worker sfu_worker_t;

typedef struct sfu_scheduler {
  sfu_ring_t recv_ring;
  sfu_packet_pool_t *pp;
  sfu_worker_t *workers;
  sfu_routing_table_t *routing_table;
  const sfu_ice_credentials_t *ice_creds;
  sfu_affinity_entry_t affinity[SFU_AFFINITY_CACHE_CAP];
  pthread_t thread;
  uint32_t worker_count;
  int core_id;
  int fd;
} sfu_scheduler_t;

int sfu_scheduler_init(sfu_scheduler_t *s, int core_id, int fd, sfu_packet_pool_t *pp, sfu_worker_t *workers, uint32_t worker_count,
                       sfu_routing_table_t *routing_table, const sfu_ice_credentials_t *ice_creds, int recv_bgid, uint32_t buf_count, uint32_t buf_size);
void sfu_scheduler_destroy(sfu_scheduler_t *s);
int sfu_scheduler_start(sfu_scheduler_t *s);
void sfu_scheduler_join(sfu_scheduler_t *s);

#endif /* SFU_RUNTIME_SCHEDULER_H */
