#include "runtime/worker.h"
#include <string.h>
#include <unistd.h>
#include "config/config.h"
#include "peer/session.h"
#include "pipeline/dispatch.h"
#include "runtime/cpu.h"
#include "runtime/fanout.h"
#include "runtime/fanout_job.h"
#include "runtime/signal.h"
#include "runtime/timer.h"
#include "util/alloc.h"
#include "util/log.h"

#define SFU_WORKER_SEND_SQ_ENTRIES 1024
#define SFU_WORKER_SEND_CQ_ENTRIES 2048
#define SFU_WORKER_REAP_BATCH 128
#define SFU_WORKER_IDLE_SLEEP_US 200
#define SFU_WORKER_TWCC_FLUSH_INTERVAL_US 15000LL

bool sfu_worker_register_session(sfu_worker_t *w, sfu_peer_session_t *s) {
  if (!w || !s) {
    return false;
  }
  pthread_mutex_lock(&w->local_sessions_lock);
  for (uint32_t i = 0; i < w->local_session_count; i++) {
    if (w->local_sessions[i] == s) {
      pthread_mutex_unlock(&w->local_sessions_lock);
      return true;
    }
  }
  if (w->local_session_count == w->local_session_capacity) {
    uint32_t capacity = w->local_session_capacity ? w->local_session_capacity * 2 : 64;
    if (capacity > SFU_SESSION_TABLE_MAX) {
      capacity = SFU_SESSION_TABLE_MAX;
    }
    if (capacity <= w->local_session_capacity) {
      pthread_mutex_unlock(&w->local_sessions_lock);
      return false;
    }
    sfu_peer_session_t **sessions = SFU_REALLOC(w->local_sessions, (size_t)capacity * sizeof(*sessions));
    if (!sessions) {
      pthread_mutex_unlock(&w->local_sessions_lock);
      return false;
    }
    w->local_sessions = sessions;
    w->local_session_capacity = capacity;
  }
  atomic_fetch_add_explicit(&s->refcount, 1, memory_order_relaxed);
  w->local_sessions[w->local_session_count++] = s;
  pthread_mutex_unlock(&w->local_sessions_lock);
  return true;
}

void sfu_worker_unregister_session(sfu_worker_t *w, sfu_peer_session_t *s) {
  if (!w || !s) {
    return;
  }
  sfu_peer_session_t *removed = NULL;
  pthread_mutex_lock(&w->local_sessions_lock);
  for (uint32_t i = 0; i < w->local_session_count; i++) {
    if (w->local_sessions[i] == s) {
      removed = w->local_sessions[i];
      w->local_sessions[i] = w->local_sessions[--w->local_session_count];
      break;
    }
  }
  pthread_mutex_unlock(&w->local_sessions_lock);
  sfu_session_release(removed);
}

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

  if (pthread_mutex_init(&w->local_sessions_lock, NULL) != 0) {
    return -1;
  }

  if (sfu_spsc_ring_init(&w->inbox, inbox_capacity) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init inbox ring", worker_index);
    pthread_mutex_destroy(&w->local_sessions_lock);
    return -1;
  }

  if (sfu_spsc_ring_init(&w->release_to_dispatcher, g_sfu_config.release_queue_capacity) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init release queue", worker_index);
    sfu_spsc_ring_destroy(&w->inbox);
    pthread_mutex_destroy(&w->local_sessions_lock);
    return -1;
  }

  if (sfu_ring_init(&w->send_ring, fd, SFU_WORKER_SEND_SQ_ENTRIES, SFU_WORKER_SEND_CQ_ENTRIES, 0, 0, send_bgid, false) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init send ring", worker_index);
    sfu_spsc_ring_destroy(&w->release_to_dispatcher);
    sfu_spsc_ring_destroy(&w->inbox);
    pthread_mutex_destroy(&w->local_sessions_lock);
    return -1;
  }

  return 0;
}

void sfu_worker_destroy(sfu_worker_t *w) {
  pthread_mutex_lock(&w->local_sessions_lock);
  for (uint32_t i = 0; i < w->local_session_count; i++) {
    sfu_session_release(w->local_sessions[i]);
  }
  SFU_FREE(w->local_sessions);
  SFU_FREE(w->twcc_scratch);
  w->local_sessions = NULL;
  w->twcc_scratch = NULL;
  w->local_session_count = 0;
  w->local_session_capacity = 0;
  w->twcc_scratch_capacity = 0;
  pthread_mutex_unlock(&w->local_sessions_lock);
  pthread_mutex_destroy(&w->local_sessions_lock);
  sfu_ring_destroy(&w->send_ring);
  sfu_spsc_ring_destroy(&w->release_to_dispatcher);
  sfu_spsc_ring_destroy(&w->inbox);
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

    int64_t now_us = (int64_t)sfu_now_us();
    bool flushed_twcc = false;
    if (now_us - w->last_twcc_flush_us >= SFU_WORKER_TWCC_FLUSH_INTERVAL_US) {
      w->last_twcc_flush_us = now_us;
      /* Snapshot and pin eligible sessions under the registry lock, then do
       * feedback/SRTP/send work without holding the control-plane mutex. */
      uint32_t twcc_count = 0;
      pthread_mutex_lock(&w->local_sessions_lock);
      if (w->twcc_scratch_capacity < w->local_session_count) {
        uint32_t capacity = w->local_session_capacity;
        sfu_peer_session_t **scratch = SFU_REALLOC(w->twcc_scratch, (size_t)capacity * sizeof(*scratch));
        if (scratch) {
          w->twcc_scratch = scratch;
          w->twcc_scratch_capacity = capacity;
        }
      }
      for (uint32_t li = 0; li < w->local_session_count;) {
        sfu_peer_session_t *ls = w->local_sessions[li];
        if (!ls || !sfu_session_accepts_work(ls) || sfu_session_owner_worker(ls) != w->worker_index) {
          w->local_sessions[li] = w->local_sessions[--w->local_session_count];
          pthread_mutex_unlock(&w->local_sessions_lock);
          sfu_session_release(ls);
          pthread_mutex_lock(&w->local_sessions_lock);
          continue;
        }
        if (twcc_count < w->twcc_scratch_capacity) {
          atomic_fetch_add_explicit(&ls->refcount, 1, memory_order_relaxed);
          w->twcc_scratch[twcc_count++] = ls;
        }
        li++;
      }
      pthread_mutex_unlock(&w->local_sessions_lock);

      for (uint32_t li = 0; li < twcc_count; li++) {
        sfu_peer_session_t *ls = w->twcc_scratch[li];
        if (sfu_session_accepts_work(ls) && sfu_session_owner_worker(ls) == w->worker_index) {
          sfu_session_maybe_send_twcc_feedback(w, ls);
          flushed_twcc = true;
        }
        sfu_session_release(ls);
      }
    }

    if (drained > 0 || fanned > 0 || flushed_twcc) {
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
