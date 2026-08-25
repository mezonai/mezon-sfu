#include "runtime/worker.h"
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include "config/config.h"
#include "peer/session.h"
#include "pipeline/dispatch.h"
#include "runtime/cpu.h"
#include "runtime/epoch_reclaimer.h"
#include "runtime/fanout.h"
#include "runtime/fanout_job.h"
#include "runtime/signal.h"
#include "runtime/timer.h"
#include "util/alloc.h"
#include "util/log.h"

#define SFU_WORKER_SEND_SQ_ENTRIES 1024
#define SFU_WORKER_SEND_CQ_ENTRIES 2048
#define SFU_WORKER_REAP_BATCH 128
#define SFU_WORKER_IDLE_SLEEP_MIN_US 200
#define SFU_WORKER_IDLE_SLEEP_MAX_US 5000
#define SFU_WORKER_TWCC_FLUSH_INTERVAL_US 15000LL
#define SFU_WORKER_REMB_SCAN_INTERVAL_US 50000LL
#ifdef SFU_DIAG_LOG
#define SFU_WORKER_DIAG_SCAN_INTERVAL_US 100000LL
#endif

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

  if (sfu_worker_packet_arena_init(&w->output_arena, SFU_WORKER_OUTPUT_ARENA_CAPACITY, g_sfu_config.packet_buf_size) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init output arena", worker_index);
    sfu_spsc_ring_destroy(&w->release_to_dispatcher);
    sfu_spsc_ring_destroy(&w->inbox);
    pthread_mutex_destroy(&w->local_sessions_lock);
    return -1;
  }

  if (sfu_ring_init(&w->send_ring, fd, SFU_WORKER_SEND_SQ_ENTRIES, SFU_WORKER_SEND_CQ_ENTRIES, 0, 0, send_bgid, false) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init send ring", worker_index);
    sfu_worker_packet_arena_destroy(&w->output_arena);
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
  if (w->output_arena.in_use != 0) {
    SFU_LOG_WARN("worker %u: destroying output arena with %u sends still outstanding", w->worker_index, w->output_arena.in_use);
  }
  sfu_worker_packet_arena_destroy(&w->output_arena);
  sfu_spsc_ring_destroy(&w->release_to_dispatcher);
  sfu_spsc_ring_destroy(&w->inbox);
}

static void *worker_thread_main(void *arg) {
  sfu_worker_t *w = (sfu_worker_t *)arg;
  sfu_pin_current_thread_to_core(w->core_id);

  SFU_LOG_INFO("worker %u started (core %d)", w->worker_index, w->core_id);

  uint32_t idle_sleep_us = SFU_WORKER_IDLE_SLEEP_MIN_US;

  uint32_t loop_counter = 0;
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
    bool scanned_remb = false;
    bool twcc_due = now_us - w->last_twcc_flush_us >= SFU_WORKER_TWCC_FLUSH_INTERVAL_US;
    bool remb_due = now_us - w->last_remb_scan_us >= SFU_WORKER_REMB_SCAN_INTERVAL_US;
#ifdef SFU_DIAG_LOG
    bool diag_due = now_us - w->last_diag_scan_us >= SFU_WORKER_DIAG_SCAN_INTERVAL_US;
#else
    bool diag_due = false;
#endif
    if (twcc_due || remb_due || diag_due) {
      if (twcc_due) {
        w->last_twcc_flush_us = now_us;
      }
      if (remb_due) {
        w->last_remb_scan_us = now_us;
      }
#ifdef SFU_DIAG_LOG
      if (diag_due) {
        w->last_diag_scan_us = now_us;
      }
#endif
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
          if (twcc_due) {
            sfu_session_maybe_send_twcc_feedback(w, ls);
            flushed_twcc = true;
          }
          if (remb_due && sfu_session_maybe_send_publisher_remb(w, ls, now_us)) {
            scanned_remb = true;
          }
#ifdef SFU_DIAG_LOG
          if (diag_due) {
            sfu_session_log_congestion_diag(w, ls, (uint64_t)now_us);
          }
#endif
        }
        sfu_session_release(ls);
      }
    }

    if (drained > 0 || fanned > 0 || flushed_twcc || scanned_remb) {
      sfu_ring_submit(&w->send_ring);
    }

    unsigned reaped = sfu_ring_reap(&w->send_ring, SFU_WORKER_REAP_BATCH, w->pp, &w->release_to_dispatcher, NULL, NULL, w);
    if (reaped > 0) {
      did_work = true;
    }

    if (++loop_counter % 100 == 0 && w->sessions && w->sessions->reclaimer) {
      uint32_t reclaimed = sfu_epoch_reclaimer_sweep(w->sessions->reclaimer);
      if (reclaimed > 0) {
        did_work = true;
      }
    }

    if (!did_work) {
      usleep(idle_sleep_us);
      if (idle_sleep_us < SFU_WORKER_IDLE_SLEEP_MAX_US) {
        idle_sleep_us = idle_sleep_us * 2;
        if (idle_sleep_us > SFU_WORKER_IDLE_SLEEP_MAX_US) {
          idle_sleep_us = SFU_WORKER_IDLE_SLEEP_MAX_US;
        }
      }
    } else {
      idle_sleep_us = SFU_WORKER_IDLE_SLEEP_MIN_US;
    }

    atomic_fetch_add_explicit(&w->generation, 1, memory_order_release);
  }

  for (unsigned idle_passes = 0; idle_passes < 32 || sfu_ring_outstanding_sends(&w->send_ring) > 0;) {
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

    if (w->sessions && w->sessions->reclaimer) {
      (void)sfu_epoch_reclaimer_sweep(w->sessions->reclaimer);
    }

    if (did_work) {
      idle_passes = 0;
    } else {
      idle_passes++;
      usleep(SFU_WORKER_IDLE_SLEEP_MIN_US);
    }

    atomic_fetch_add_explicit(&w->generation, 1, memory_order_release);
  }

  if (w->sessions && w->sessions->reclaimer) {
    (void)sfu_epoch_reclaimer_sweep(w->sessions->reclaimer);
  }

  atomic_store_explicit(&w->drain_finished, true, memory_order_release);
  SFU_LOG_INFO("worker %u fanout stats: arena_alloc=%" PRIu64 " reserved=%" PRIu64 " fallback=%" PRIu64 " queued=%" PRIu64
               " recycled=%" PRIu64 " exhausted=%" PRIu64 " high_water=%u copied_bytes=%" PRIu64 " samples=%" PRIu64
               " copy_cycles=%" PRIu64 " crypto_cycles=%" PRIu64,
               w->worker_index, w->hot.output_arena_allocated, w->hot.output_reserved, w->hot.output_pool_fallback, w->hot.output_queued,
               w->output_arena.recycled, w->output_arena.exhausted, w->output_arena.high_water, w->hot.copied_bytes, w->hot.profile_samples,
               w->hot.copy_cycles, w->hot.crypto_cycles);
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

bool sfu_worker_drain_finished(const sfu_worker_t *w) {
  return w && atomic_load_explicit(&w->drain_finished, memory_order_acquire);
}
