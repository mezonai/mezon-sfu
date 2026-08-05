#include "runtime/worker.h"
#include <string.h>
#include <unistd.h>
#include "config/config.h"
#include "peer/session.h"
#include "pipeline/dispatch.h"
#include "pipeline/egress.h"
#include "runtime/cpu.h"
#include "runtime/fanout.h"
#include "runtime/signal.h"
#include "util/log.h"

#define SFU_WORKER_SEND_SQ_ENTRIES 1024
#define SFU_WORKER_SEND_CQ_ENTRIES 2048
#define SFU_WORKER_REAP_BATCH 128
#define SFU_WORKER_IDLE_SLEEP_US 200

int sfu_worker_init(sfu_worker_t *w, int core_id, uint32_t worker_index, int fd, sfu_packet_pool_t *pp, sfu_room_registry_t *room_registry,
                    sfu_fanout_mesh_t *mesh, sfu_session_table_t *sessions, sfu_routing_table_t *routing_table, const sfu_ice_credentials_t *ice_creds,
                    sfu_scheduler_t *scheduler, uint32_t inbox_capacity, int send_bgid) {
  memset(w, 0, sizeof(*w));
  w->core_id = core_id;
  w->worker_index = worker_index;
  w->fd = fd;
  w->pp = pp;
  w->room_registry = room_registry;
  w->mesh = mesh;
  w->sessions = sessions;
  w->ice_creds = ice_creds;
  w->routing_table = routing_table;
  w->scheduler = scheduler;

  if (sfu_spsc_ring_init(&w->inbox, inbox_capacity) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init inbox ring", worker_index);
    return -1;
  }

  if (sfu_spsc_ring_init(&w->release_to_dispatcher, g_sfu_config.release_queue_capacity) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init release queue", worker_index);
    sfu_spsc_ring_destroy(&w->inbox);
    return -1;
  }

  if (sfu_ring_init(&w->send_ring, fd, SFU_WORKER_SEND_SQ_ENTRIES, SFU_WORKER_SEND_CQ_ENTRIES, 0, 0, send_bgid, false) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init send ring", worker_index);
    sfu_spsc_ring_destroy(&w->release_to_dispatcher);
    sfu_spsc_ring_destroy(&w->inbox);
    return -1;
  }

  return 0;
}

void sfu_worker_destroy(sfu_worker_t *w) {
  sfu_ring_destroy(&w->send_ring);
  sfu_spsc_ring_destroy(&w->release_to_dispatcher);
  sfu_spsc_ring_destroy(&w->inbox);
}

void sfu_worker_request_keyframe_throttled(sfu_worker_t *w, sfu_peer_session_t *publisher) {
  sfu_session_request_keyframe(w, publisher, false);
}

void sfu_worker_handle_fanout_job(void *user_data, sfu_fanout_job_t *job) {
  sfu_worker_t *w = (sfu_worker_t *)user_data;

  if (job->kind == SFU_FANOUT_JOB_KEYFRAME_REQUEST) {
    if (job->publisher) {
      sfu_session_request_keyframe(w, job->publisher, false);
      sfu_session_release(job->publisher);
    }
    sfu_fanout_mesh_free_job(w->mesh, job);
    return;
  } else if (job->kind == SFU_FANOUT_JOB_FORWARD && job->subscriber) {
    sfu_egress_process(w, job->subscriber, job->pkt, &job->dst, job->dst_len, job->video_ssrc, job->video_pt, job->video_rtx_pt, job->video_rtx_ssrc,
                       job->has_video, job->is_audio, (sfu_pacer_class_t)job->pacer_class);
    sfu_session_release(job->subscriber);
  } else {
    if (sfu_ring_queue_send_zc(&w->send_ring, job->pkt, (const struct sockaddr *)&job->dst, job->dst_len) != 0) {
      SFU_LOG_WARN("worker %u: [EGRESS DROP] remote-fanout send SQ full, dropping packet", w->worker_index);
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, job->pkt);
  sfu_fanout_mesh_free_job(w->mesh, job);
}

static void *worker_thread_main(void *arg) {
  sfu_worker_t *w = (sfu_worker_t *)arg;
  sfu_pin_current_thread_to_core(w->core_id);

  SFU_LOG_INFO("worker %u started (core %d)", w->worker_index, w->core_id);

  while (!sfu_shutdown_requested()) {
    bool did_work = false;

    void *item;
    int drained = 0;
    while (drained < SFU_WORKER_REAP_BATCH && sfu_spsc_ring_pop(&w->inbox, &item)) {
      sfu_dispatch_packet(w, (sfu_packet_t *)item);
      drained++;
      did_work = true;
    }

    unsigned fanned = sfu_fanout_mesh_drain(w->mesh, w->worker_index, SFU_WORKER_REAP_BATCH, sfu_worker_handle_fanout_job, w);
    if (fanned > 0) {
      did_work = true;
    }

    if (drained > 0 || fanned > 0) {
      sfu_ring_submit(&w->send_ring);
    }

    unsigned reaped = sfu_ring_reap(&w->send_ring, SFU_WORKER_REAP_BATCH, w->pp, &w->release_to_dispatcher, NULL, NULL, w);
    if (reaped > 0) {
      did_work = true;
    }

    if (!did_work) {
      usleep(SFU_WORKER_IDLE_SLEEP_US);
    }

    __atomic_fetch_add(&w->generation, 1, __ATOMIC_RELEASE);
  }

  for (unsigned idle_passes = 0; idle_passes < 32;) {
    bool did_work = false;

    void *item;
    while (sfu_spsc_ring_pop(&w->inbox, &item)) {
      sfu_dispatch_packet(w, (sfu_packet_t *)item);
      did_work = true;
    }

    if (sfu_fanout_mesh_drain(w->mesh, w->worker_index, SFU_WORKER_REAP_BATCH, sfu_worker_handle_fanout_job, w) > 0) {
      did_work = true;
    }

    if (sfu_ring_submit(&w->send_ring) > 0) {
      did_work = true;
    }

    if (sfu_ring_reap(&w->send_ring, SFU_WORKER_REAP_BATCH, w->pp, &w->release_to_dispatcher, NULL, NULL, w) > 0) {
      did_work = true;
    }

    if (did_work) {
      idle_passes = 0;
    } else {
      idle_passes++;
      usleep(SFU_WORKER_IDLE_SLEEP_US);
    }

    __atomic_fetch_add(&w->generation, 1, __ATOMIC_RELEASE);
  }

  SFU_LOG_INFO("worker %u shutting down", w->worker_index);
  return NULL;
}

int sfu_worker_start(sfu_worker_t *w) {
  int rc = pthread_create(&w->thread, NULL, worker_thread_main, w);
  if (rc != 0) {
    SFU_LOG_ERROR("worker %d: pthread_create failed: %d", w->core_id, rc);
    return -1;
  }
  return 0;
}

void sfu_worker_join(sfu_worker_t *w) { pthread_join(w->thread, NULL); }
