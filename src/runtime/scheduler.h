#ifndef SFU_RUNTIME_SCHEDULER_H
#define SFU_RUNTIME_SCHEDULER_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "runtime/worker.h"

typedef struct sfu_deferred_free {
  void *ptr;
  uint32_t worker_count;
  uint64_t *worker_generations;
  struct sfu_deferred_free *next;
} sfu_deferred_free_t;

typedef struct sfu_scheduler {
  int core_id;
  sfu_ring_t recv_ring;
  sfu_packet_pool_t *pp;

  sfu_worker_t *workers;
  uint32_t worker_count;

  pthread_t thread;
  int fd;

  sfu_deferred_free_t *pending_free_head;
  pthread_mutex_t pending_free_lock;
  struct timespec last_sweep;
} sfu_scheduler_t;

int sfu_scheduler_init(sfu_scheduler_t *s, int core_id, int fd, sfu_packet_pool_t *pp, sfu_worker_t *workers, uint32_t worker_count, int recv_bgid,
                       uint32_t buf_count, uint32_t buf_size);
void sfu_scheduler_destroy(sfu_scheduler_t *s);

int sfu_scheduler_start(sfu_scheduler_t *s);
void sfu_scheduler_join(sfu_scheduler_t *s);

void sfu_scheduler_retire_ptr(sfu_scheduler_t *s, void *ptr);

#endif /* SFU_RUNTIME_SCHEDULER_H */
