#include "runtime/scheduler.h"
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "runtime/cpu.h"
#include "runtime/signal.h"
#include "runtime/worker.h"
#include "sfu/datadef.h"
#include "util/alloc.h"
#include "util/log.h"

#define SFU_DISPATCH_SQ_ENTRIES 1024
#define SFU_DISPATCH_CQ_ENTRIES 4096
#define SFU_DISPATCH_REAP_BATCH 256
#define SFU_DISPATCH_IDLE_SLEEP_US 100

#define SFU_PENDING_FREE_SWEEP_INTERVAL_SEC 1

int sfu_scheduler_init(sfu_scheduler_t *s, int core_id, int fd, sfu_packet_pool_t *pp, sfu_worker_t *workers, uint32_t worker_count, int recv_bgid,
                       uint32_t buf_count, uint32_t buf_size) {
  memset(s, 0, sizeof(*s));
  s->core_id = core_id;
  s->fd = fd;
  s->pp = pp;
  s->workers = workers;
  s->worker_count = worker_count;

  if (sfu_ring_init(&s->recv_ring, fd, SFU_DISPATCH_SQ_ENTRIES, SFU_DISPATCH_CQ_ENTRIES, buf_count, buf_size, recv_bgid, true) != 0) {
    SFU_LOG_ERROR("scheduler: failed to init recv ring");
    return -1;
  }

  s->pending_free_head = NULL;
  if (pthread_mutex_init(&s->pending_free_lock, NULL) != 0) {
    SFU_LOG_ERROR("scheduler: failed to init pending_free_lock");
    sfu_ring_destroy(&s->recv_ring);
    return -1;
  }
  clock_gettime(CLOCK_MONOTONIC, &s->last_sweep);

  return 0;
}

void sfu_scheduler_destroy(sfu_scheduler_t *s) {
  sfu_ring_destroy(&s->recv_ring);

  sfu_deferred_free_t *df = s->pending_free_head;
  while (df) {
    sfu_deferred_free_t *next = df->next;
    SFU_FREE(df->ptr);
    SFU_FREE(df->worker_generations);
    SFU_FREE(df);
    df = next;
  }
  s->pending_free_head = NULL;
  pthread_mutex_destroy(&s->pending_free_lock);
}

static void sfu_scheduler_sync_reclaim(sfu_scheduler_t *s, void *ptr) {
  uint64_t fallback_generations[SFU_MAX_WORKERS];
  for (uint32_t i = 0; i < s->worker_count && i < SFU_MAX_WORKERS; i++) {
    fallback_generations[i] = __atomic_load_n(&s->workers[i].generation, __ATOMIC_ACQUIRE);
  }
  for (uint32_t i = 0; i < s->worker_count && i < SFU_MAX_WORKERS; i++) {
    uint32_t attempts = 0;
    while (__atomic_load_n(&s->workers[i].generation, __ATOMIC_ACQUIRE) == fallback_generations[i]) {
      usleep(100);
      if (++attempts >= 10000) {
        SFU_LOG_ERROR("scheduler: worker %u generation wait timeout during synchronous reclaim", i);
        break;
      }
    }
  }
  SFU_FREE(ptr);
}

void sfu_scheduler_retire_ptr(sfu_scheduler_t *s, void *ptr) {
  sfu_deferred_free_t *df = SFU_CALLOC(1, sizeof(*df));
  if (!df) {
    SFU_LOG_WARN("scheduler: deferred-free allocation failed; reclaiming synchronously");
    sfu_scheduler_sync_reclaim(s, ptr);
    return;
  }

  df->ptr = ptr;
  df->worker_count = s->worker_count;
  df->worker_generations = SFU_CALLOC(s->worker_count, sizeof(uint64_t));
  if (!df->worker_generations) {
    SFU_LOG_WARN("scheduler: generation snapshot allocation failed; reclaiming synchronously");
    SFU_FREE(df);
    sfu_scheduler_sync_reclaim(s, ptr);
    return;
  }
  for (uint32_t i = 0; i < s->worker_count; i++) {
    df->worker_generations[i] = __atomic_load_n(&s->workers[i].generation, __ATOMIC_ACQUIRE);
  }

  pthread_mutex_lock(&s->pending_free_lock);
  df->next = s->pending_free_head;
  s->pending_free_head = df;
  pthread_mutex_unlock(&s->pending_free_lock);
}

static void sweep_pending_frees(sfu_scheduler_t *s) {
  if (!s->pending_free_head) {
    return;
  }

  pthread_mutex_lock(&s->pending_free_lock);

  sfu_deferred_free_t **cursor = &s->pending_free_head;
  while (*cursor) {
    sfu_deferred_free_t *df = *cursor;

    bool safe = true;
    for (uint32_t i = 0; i < df->worker_count; i++) {
      uint64_t current = __atomic_load_n(&s->workers[i].generation, __ATOMIC_ACQUIRE);
      if (current == df->worker_generations[i]) {
        safe = false;
        break;
      }
    }

    if (safe) {
      *cursor = df->next;
      SFU_FREE(df->ptr);
      SFU_FREE(df->worker_generations);
      SFU_FREE(df);
    } else {
      cursor = &df->next;
    }
  }

  pthread_mutex_unlock(&s->pending_free_lock);
}

typedef struct {
  sfu_scheduler_t *s;
} recv_ctx_t;

static void on_recv(void *user_data, sfu_packet_t *pkt) {
  sfu_scheduler_t *s = ((recv_ctx_t *)user_data)->s;

  uint32_t h = fnv1a(&pkt->peer_addr, pkt->peer_addr_len);
  uint32_t worker_idx = h % s->worker_count;

  if (!sfu_spsc_ring_push(&s->workers[worker_idx].inbox, pkt)) {
    SFU_LOG_WARN("worker %u inbox full, dropping packet", worker_idx);
    sfu_ring_release_packet(&s->recv_ring, s->pp, pkt);
  }
}

static void *scheduler_thread_main(void *arg) {
  sfu_scheduler_t *s = (sfu_scheduler_t *)arg;
  sfu_pin_current_thread_to_core(s->core_id);

  recv_ctx_t ctx = {.s = s};

  if (sfu_ring_arm_recv(&s->recv_ring) != 0) {
    SFU_LOG_ERROR("scheduler: failed to arm initial recv");
    return NULL;
  }
  sfu_ring_submit(&s->recv_ring);

  SFU_LOG_INFO("scheduler (dispatcher) started on core %d", s->core_id);

  while (!sfu_shutdown_requested()) {
    unsigned reaped = sfu_ring_reap(&s->recv_ring, SFU_DISPATCH_REAP_BATCH, s->pp, NULL, on_recv, NULL, &ctx);

    unsigned returned = 0;
    for (uint32_t i = 0; i < s->worker_count; i++) {
      returned += sfu_ring_drain_kernel_buffer_returns(&s->recv_ring, &s->workers[i].release_to_dispatcher, SFU_DISPATCH_REAP_BATCH);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - s->last_sweep.tv_sec >= SFU_PENDING_FREE_SWEEP_INTERVAL_SEC) {
      sweep_pending_frees(s);
      s->last_sweep = now;
    }

    if (reaped == 0 && returned == 0) {
      usleep(SFU_DISPATCH_IDLE_SLEEP_US);
    }
  }

  SFU_LOG_INFO("scheduler shutting down");
  return NULL;
}

int sfu_scheduler_start(sfu_scheduler_t *s) {
  int rc = pthread_create(&s->thread, NULL, scheduler_thread_main, s);
  if (rc != 0) {
    SFU_LOG_ERROR("scheduler: pthread_create failed: %d", rc);
    return -1;
  }
  return 0;
}

void sfu_scheduler_join(sfu_scheduler_t *s) { pthread_join(s->thread, NULL); }

void sfu_subscriber_scheduler_init(sfu_subscriber_scheduler_t *sched, uint32_t initial_publisher) {
  sched->active_publisher_id = initial_publisher;
  sched->is_pinned = false;
  sched->target_sid = 0;
  sched->target_tid = 0;
  sched->current_sid = 0;
  sched->current_tid = 0;
  sched->needs_keyframe = true;  // Wait for an I-frame to start decoding
}

bool sfu_scheduler_evaluate_frame(sfu_subscriber_scheduler_t *sched, const sfu_vp9_descriptor_t *desc, bool is_keyframe) {
  if (sched->needs_keyframe) {
    if (!is_keyframe) {
      return false;  // Drop everything until we get a clean start
    }
    sched->needs_keyframe = false;
    sched->current_sid = sched->target_sid;
    sched->current_tid = sched->target_tid;
  }

  if (sched->target_sid < sched->current_sid) {
    sched->current_sid = sched->target_sid;
  }
  if (sched->target_tid < sched->current_tid) {
    sched->current_tid = sched->target_tid;
  }

  if (sched->current_sid < sched->target_sid) {
    // In VP9, P=0 means no inter-picture prediction (Keyframe or Spatial Sync)
    if (desc->p_bit == 0 && desc->sid <= sched->target_sid) {
      sched->current_sid = desc->sid;  // Upshift progressively
    }
  }

  if (sched->current_tid < sched->target_tid) {
    // In VP9, U=1 means it is a valid switching point for temporal layers
    if (desc->u_bit == 1 && desc->tid <= sched->target_tid) {
      sched->current_tid = desc->tid;
    }
  }

  if (desc->sid > sched->current_sid) {
    return false;  // Drop higher spatial layers
  }
  if (desc->tid > sched->current_tid) {
    return false;  // Drop higher temporal layers
  }

  // Drop inter-layer dependent frames if we aren't subscribing to the base layer they need
  if (desc->d_bit == 1 && desc->sid > sched->current_sid) {
    return false;
  }

  return true;  // Frame is required by this subscriber
}
