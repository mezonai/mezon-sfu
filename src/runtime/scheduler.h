#ifndef SFU_RUNTIME_SCHEDULER_H
#define SFU_RUNTIME_SCHEDULER_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "media/svc/vp9_parser.h"
#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "runtime/epoch_reclaimer.h"

typedef struct sfu_worker sfu_worker_t;

typedef struct sfu_subscriber_scheduler {
  uint32_t active_publisher_id;
  bool is_pinned;
  uint8_t target_sid;
  uint8_t target_tid;
  uint8_t current_sid;
  uint8_t current_tid;
  bool needs_keyframe;
} sfu_subscriber_scheduler_t;

typedef struct sfu_scheduler {
  sfu_ring_t recv_ring;
  sfu_epoch_reclaimer_t reclaimer;
  struct timespec last_sweep;
  sfu_packet_pool_t *pp;
  sfu_worker_t *workers;
  pthread_t thread;
  uint32_t worker_count;
  int core_id;
  int fd;
} sfu_scheduler_t;

int sfu_scheduler_init(sfu_scheduler_t *s, int core_id, int fd, sfu_packet_pool_t *pp, sfu_worker_t *workers, uint32_t worker_count, int recv_bgid,
                       uint32_t buf_count, uint32_t buf_size);
void sfu_scheduler_destroy(sfu_scheduler_t *s);
int sfu_scheduler_start(sfu_scheduler_t *s);
void sfu_scheduler_join(sfu_scheduler_t *s);
/* Returns false without freeing ptr; ownership then remains with the caller. */
bool sfu_scheduler_retire_ptr(sfu_scheduler_t *s, void *ptr);
void sfu_subscriber_scheduler_init(sfu_subscriber_scheduler_t *sched, uint32_t initial_publisher);
bool sfu_scheduler_evaluate_frame(sfu_subscriber_scheduler_t *sched, const sfu_vp9_descriptor_t *desc, bool is_keyframe);

/* Maps a congestion-control bitrate estimate onto the exact target SID/TID
 * fields consumed by sfu_scheduler_evaluate_frame (CC-02). This is the single
 * control entry point for GCC output: it writes the scheduler the forwarding
 * hot path actually reads, never the duplicate session-level fields. */
void sfu_subscriber_scheduler_set_bitrate(sfu_subscriber_scheduler_t *sched, uint32_t bitrate_bps);

#endif /* SFU_RUNTIME_SCHEDULER_H */
