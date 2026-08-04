#ifndef SFU_RUNTIME_SCHEDULER_H
#define SFU_RUNTIME_SCHEDULER_H

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "congestion/pacer.h"
#include "media/svc/vp9_parser.h"
#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "runtime/epoch_reclaimer.h"

typedef struct sfu_worker sfu_worker_t;
typedef struct sfu_peer_session sfu_peer_session_t;

/* Per-subscriber, per-publisher-track scheduler state. VP9 spatial/temporal
 * layer tracking is per (subscriber, publisher) stream: frames from two
 * different publishers must never be evaluated through the same instance, or
 * the current_sid/current_tid state becomes corrupted.
 *
 * The pacer is NOT here: pacing is a per-subscriber egress-path concern (one
 * GCC estimate, one outbound token bucket), so it lives on the session. This
 * struct carries only per-track layer-selection state. */
typedef struct sfu_subscriber_scheduler {
  uint32_t active_publisher_id;
  bool is_pinned;
  uint8_t target_sid;
  uint8_t target_tid;
  uint8_t current_sid;
  uint8_t current_tid;
  bool needs_keyframe;
  /* Last time the target layers changed (microseconds); enforces dwell time
   * between target changes so a jittery GCC estimate cannot flap layers. */
  int64_t last_target_change_us;
} sfu_subscriber_scheduler_t;

/* Capacity of the per-session subscriber scheduler table. One slot per
 * distinct publisher video stream the session is subscribed to. */
#define SFU_SESSION_SCHEDULER_CAP 8

/* A slot in the session's scheduler table: the per-publisher scheduler plus
 * the publisher_id it is keyed by. publisher_id == 0 means the slot is empty
 * (peer_ids are generated non-zero). */
typedef struct sfu_session_scheduler_slot {
  uint32_t publisher_id;
  sfu_subscriber_scheduler_t sched;
} sfu_session_scheduler_slot_t;

/* Looks up (or lazily creates and initializes) the scheduler for the given
 * publisher on this subscriber session. The table lives on the subscriber
 * session and is owned by that session's single owning worker; entries are
 * created on first use and never freed individually (session teardown frees
 * the whole table). Returns NULL only when the table is full. */
sfu_subscriber_scheduler_t *sfu_session_scheduler_for(sfu_peer_session_t *session, uint32_t publisher_id);

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

sfu_pacer_class_t sfu_scheduler_classify_frame(const sfu_subscriber_scheduler_t *sched, const sfu_vp9_descriptor_t *desc);

/* Maps a congestion-control bitrate estimate onto the exact target SID/TID
 * fields consumed by sfu_scheduler_evaluate_frame (CC-02). This is the single
 * control entry point for GCC output: it writes the scheduler the forwarding
 * hot path actually reads, never the duplicate session-level fields. */
void sfu_subscriber_scheduler_set_bitrate(sfu_subscriber_scheduler_t *sched, uint32_t bitrate_bps);

/* Source-switch transaction (#83): re-aims the per-publisher selector at
 * `new_publisher_id`, resets its layer state with the keyframe gate armed,
 * bumps the session's egress generation (invalidating stale RTX entries,
 * F-10), and restarts the GCC estimator from its configured bounds. Call from
 * the session's owning worker only; the caller is responsible for requesting
 * a keyframe from the new source. */
void sfu_layer_selector_switch_source(sfu_peer_session_t *session, uint32_t new_publisher_id);

#endif /* SFU_RUNTIME_SCHEDULER_H */
