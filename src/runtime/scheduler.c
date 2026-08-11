#include "runtime/scheduler.h"
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "peer/session.h"
#include "runtime/cpu.h"
#include "runtime/signal.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "sfu/datadef.h"
#include "util/alloc.h"
#include "util/log.h"

#define SFU_DISPATCH_SQ_ENTRIES 1024
#define SFU_DISPATCH_CQ_ENTRIES 4096
#define SFU_DISPATCH_REAP_BATCH 256
#define SFU_DISPATCH_IDLE_SLEEP_US 100

#define SFU_PENDING_FREE_SWEEP_INTERVAL_SEC 1

static uint64_t scheduler_worker_generation(void *context, uint32_t worker_index) {
  sfu_worker_t *workers = context;
  return __atomic_load_n(&workers[worker_index].generation, __ATOMIC_ACQUIRE);
}

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

  if (sfu_epoch_reclaimer_init(&s->reclaimer, worker_count, scheduler_worker_generation, workers) != 0) {
    sfu_ring_destroy(&s->recv_ring);
    return -1;
  }
  clock_gettime(CLOCK_MONOTONIC, &s->last_sweep);

  return 0;
}

static void scheduler_free(void *ptr) { SFU_FREE(ptr); }

void sfu_scheduler_destroy(sfu_scheduler_t *s) {
  sfu_ring_destroy(&s->recv_ring);
  sfu_epoch_reclaimer_destroy_after_quiescence(&s->reclaimer);
}

bool sfu_scheduler_retire_ptr(sfu_scheduler_t *s, void *ptr) {
  bool retired = sfu_epoch_reclaimer_retire(&s->reclaimer, ptr, scheduler_free);
  if (!retired) {
    SFU_LOG_ERROR("scheduler: retire pool exhausted; caller retains ptr=%p", ptr);
  }
  return retired;
}

typedef struct {
  sfu_scheduler_t *s;
} recv_ctx_t;

static void on_recv(void *user_data, sfu_packet_t *pkt) {
  sfu_scheduler_t *s = ((recv_ctx_t *)user_data)->s;

  if (pkt->recv_ts_ns == 0) {
    pkt->recv_ts_ns = sfu_now_ns();
  }

  if (SFU_UNLIKELY(s->worker_count == 0)) {
    SFU_LOG_ERROR("scheduler: worker_count is 0; dropping packet (misconfiguration)");
    sfu_ring_release_packet(&s->recv_ring, s->pp, pkt);
    return;
  }

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
      sfu_epoch_reclaimer_sweep(&s->reclaimer);
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
  memset(sched, 0, sizeof(*sched));
  sched->active_publisher_id = initial_publisher;
  sched->needs_keyframe = true;
}

sfu_subscriber_scheduler_t *sfu_session_scheduler_for(sfu_peer_session_t *session, uint32_t publisher_id) {
  if (!session || !session->schedulers || publisher_id == 0) {
    return NULL;
  }

  sfu_session_scheduler_slot_t *free_slot = NULL;
  for (uint32_t i = 0; i < SFU_SESSION_SCHEDULER_CAP; i++) {
    sfu_session_scheduler_slot_t *slot = &session->schedulers[i];
    if (slot->publisher_id == publisher_id) {
      return &slot->sched;
    }
    if (!free_slot && slot->publisher_id == 0) {
      free_slot = slot;
    }
  }

  if (!free_slot) {
    SFU_LOG_WARN("session %u: scheduler table full (%d publishers); cannot track publisher %u", session->peer_id, SFU_SESSION_SCHEDULER_CAP, publisher_id);
    return NULL;
  }

  free_slot->publisher_id = publisher_id;
  sfu_subscriber_scheduler_init(&free_slot->sched, publisher_id);
  return &free_slot->sched;
}

static void sfu_scheduler_begin_picture(sfu_subscriber_scheduler_t *sched, uint32_t rtp_timestamp) {
  if (sched->picture_valid && sched->picture_timestamp == rtp_timestamp) {
    return;
  }
  sched->picture_valid = true;
  sched->picture_timestamp = rtp_timestamp;
  sched->completed_sid_mask = 0;
  sched->failed_sid_mask = 0;
  sched->transition_active = false;
  sched->transition_failed = false;
  sched->transition_sid = 0;
  sched->transition_timestamp = 0;
}

sfu_pacer_class_t sfu_scheduler_classify_frame(const sfu_subscriber_scheduler_t *sched, const sfu_svc_descriptor_t *desc) {
  (void)sched;
  if (desc->sid > 0 || desc->tid > 0) {
    return SFU_PACER_CLASS_VIDEO_ENH;
  }
  return SFU_PACER_CLASS_VIDEO_BASE;
}

bool sfu_scheduler_prepare_packet(sfu_subscriber_scheduler_t *sched, const sfu_svc_descriptor_t *desc, bool is_keyframe, sfu_scheduler_decision_t *decision) {
  if (!sched || !desc || !decision) {
    return false;
  }

  memset(decision, 0, sizeof(*decision));
  decision->rtp_timestamp = desc->rtp_timestamp;
  decision->sid = desc->sid;
  decision->tid = desc->tid;
  decision->e_bit = desc->e_bit;
  decision->pacer_class = sfu_scheduler_classify_frame(sched, desc);

  sfu_scheduler_begin_picture(sched, desc->rtp_timestamp);

  if (sched->target_sid < sched->current_sid) {
    sched->current_sid = sched->target_sid;
  }
  if (sched->target_tid < sched->current_tid) {
    sched->current_tid = sched->target_tid;
  }

  if (sched->needs_keyframe) {
    if (!is_keyframe) {
      return false;
    }
    decision->start_keyframe = true;
    decision->transition_packet = true;
  }

  if (desc->sid > sched->target_sid || desc->tid > sched->target_tid) {
    return false;
  }

  if (desc->sid > sched->current_sid) {
    uint8_t candidate_sid = (uint8_t)(sched->current_sid + 1);
    if (desc->sid != candidate_sid || candidate_sid > sched->target_sid) {
      return false;
    }

    if (sched->transition_active) {
      if (sched->transition_timestamp != desc->rtp_timestamp || sched->transition_sid != desc->sid || sched->transition_failed) {
        return false;
      }
    } else {
      if (desc->b_bit == 0 || desc->p_bit != 0) {
        return false;
      }
      uint8_t lower_mask = (uint8_t)(1u << sched->current_sid);
      if (desc->d_bit != 0 && ((sched->completed_sid_mask & lower_mask) == 0 || (sched->failed_sid_mask & lower_mask) != 0)) {
        return false;
      }
      decision->start_transition = true;
    }
    decision->transition_packet = true;
  }

  if (desc->tid > sched->current_tid) {
    if (desc->u_bit == 0) {
      return false;
    }
    decision->start_temporal_transition = true;
    decision->transition_packet = true;
  }

  if (sched->target_sid > sched->current_sid && desc->p_bit == 0) {
    decision->transition_packet = true;
  }
  if (decision->transition_packet) {
    decision->pacer_class = SFU_PACER_CLASS_VIDEO_TRANSITION;
  }

  decision->should_forward = true;
  return true;
}

void sfu_scheduler_commit_packet(sfu_subscriber_scheduler_t *sched, const sfu_scheduler_decision_t *decision) {
  if (!sched || !decision || !decision->should_forward || !sched->picture_valid || sched->picture_timestamp != decision->rtp_timestamp) {
    return;
  }

  if (decision->start_keyframe) {
    sched->needs_keyframe = false;
  }
  if (decision->start_transition) {
    sched->transition_active = true;
    sched->transition_failed = false;
    sched->transition_sid = decision->sid;
    sched->transition_timestamp = decision->rtp_timestamp;
  }
  if (decision->start_temporal_transition && decision->tid <= sched->target_tid) {
    sched->current_tid = decision->tid;
  }

  uint8_t sid_mask = (uint8_t)(1u << decision->sid);
  if (decision->e_bit != 0 && (sched->failed_sid_mask & sid_mask) == 0) {
    sched->completed_sid_mask |= sid_mask;
  }

  if (sched->transition_active && sched->transition_timestamp == decision->rtp_timestamp && sched->transition_sid == decision->sid && decision->e_bit != 0) {
    if (!sched->transition_failed && (sched->failed_sid_mask & sid_mask) == 0 && decision->sid <= sched->target_sid) {
      sched->current_sid = decision->sid;
    }
    sched->transition_active = false;
    sched->transition_failed = false;
  }
}

void sfu_scheduler_reject_packet(sfu_subscriber_scheduler_t *sched, const sfu_scheduler_decision_t *decision) {
  if (!sched || !decision || !sched->picture_valid || sched->picture_timestamp != decision->rtp_timestamp || decision->sid >= 8) {
    return;
  }
  sched->failed_sid_mask |= (uint8_t)(1u << decision->sid);
  if (sched->transition_active && sched->transition_timestamp == decision->rtp_timestamp && sched->transition_sid == decision->sid) {
    sched->transition_failed = true;
  }
}

typedef struct sfu_layer_rung {
  uint32_t rate_bps;
  uint8_t sid;
  uint8_t tid;
} sfu_layer_rung_t;

static const sfu_layer_rung_t k_layer_ladder[] = {
    {150000, 0, 1},  /* 180p, half framerate */
    {500000, 1, 2},  /* 360p, full framerate */
    {1200000, 2, 2}, /* 720p, full framerate */
};
#define SFU_LAYER_LADDER_LEN (sizeof(k_layer_ladder) / sizeof(k_layer_ladder[0]))
#define SFU_LAYER_UP_HEADROOM_NUM 6 /* up threshold = rate * 1.2 */
#define SFU_LAYER_UP_HEADROOM_DEN 5
#define SFU_LAYER_DWELL_US 500000LL

void sfu_subscriber_scheduler_set_bitrate(sfu_subscriber_scheduler_t *sched, uint32_t bitrate_bps) {
  uint8_t target_sid = 0, target_tid = 0;
  int chosen = -1;

  for (int i = (int)SFU_LAYER_LADDER_LEN - 1; i >= 0; i--) {
    uint64_t up = (uint64_t)k_layer_ladder[i].rate_bps * SFU_LAYER_UP_HEADROOM_NUM / SFU_LAYER_UP_HEADROOM_DEN;
    if (bitrate_bps >= up) {
      chosen = i;
      break;
    }
  }
  if (chosen >= 0) {
    target_sid = k_layer_ladder[chosen].sid;
    target_tid = k_layer_ladder[chosen].tid;
  }

  if (sched->last_target_change_us != 0 && chosen < (int)SFU_LAYER_LADDER_LEN - 1) {
    int current_rung = -1;
    for (int i = (int)SFU_LAYER_LADDER_LEN - 1; i >= 0; i--) {
      if (sched->target_sid >= k_layer_ladder[i].sid && sched->target_tid >= k_layer_ladder[i].tid) {
        current_rung = i;
        break;
      }
    }
    if (current_rung > chosen && current_rung >= 0 && bitrate_bps >= k_layer_ladder[current_rung].rate_bps) {
      chosen = current_rung;
      target_sid = k_layer_ladder[chosen].sid;
      target_tid = k_layer_ladder[chosen].tid;
    }
  }

  if (target_sid == sched->target_sid && target_tid == sched->target_tid) {
    return;
  }

  int64_t now = (int64_t)sfu_now_us();
  if (sched->last_target_change_us != 0 && now - sched->last_target_change_us < SFU_LAYER_DWELL_US) {
    return;
  }
  sched->last_target_change_us = now;
  sched->target_sid = target_sid;
  sched->target_tid = target_tid;
}

void sfu_layer_selector_switch_source(sfu_peer_session_t *session, uint32_t new_publisher_id) {
  sfu_subscriber_scheduler_t *sched = sfu_session_scheduler_for(session, new_publisher_id);
  if (!sched) {
    return;
  }

  sched->active_publisher_id = new_publisher_id;
  sched->current_sid = 0;
  sched->current_tid = 0;
  sched->needs_keyframe = true;
  sched->picture_valid = false;
  sched->completed_sid_mask = 0;
  sched->failed_sid_mask = 0;
  sched->transition_active = false;
  sched->transition_failed = false;

  atomic_fetch_add_explicit(&session->egress_generation, 1, memory_order_acq_rel);
}
