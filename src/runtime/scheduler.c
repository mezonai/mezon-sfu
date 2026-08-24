#include "runtime/scheduler.h"
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "runtime/cpu.h"
#include "runtime/signal.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "util/log.h"
#include "util/metrics.h"

#define SFU_DISPATCH_SQ_ENTRIES 1024
#define SFU_DISPATCH_CQ_ENTRIES 4096
#define SFU_DISPATCH_REAP_BATCH 256
#define SFU_DISPATCH_IDLE_SLEEP_MIN_US 100
#define SFU_DISPATCH_IDLE_SLEEP_MAX_US 5000
#define SFU_SMALL_ROOM_AFFINITY_MAX 8

static bool affinity_addr_equal(const sfu_affinity_entry_t *entry, uint32_t hash, const struct sockaddr_storage *addr, socklen_t addr_len) {
  return entry->valid && entry->hash == hash && entry->addr_len == addr_len && memcmp(&entry->addr, addr, addr_len) == 0;
}

static uint32_t scheduler_select_worker(sfu_scheduler_t *s, sfu_packet_t *pkt, uint32_t hash) {
  uint32_t fallback = hash % s->worker_count;
  bool is_stun = pkt->data && sfu_stun_is_stun_packet(pkt->data, pkt->len);
  sfu_affinity_entry_t *entry = &s->affinity[hash & (SFU_AFFINITY_CACHE_CAP - 1)];
  bool cache_hit = affinity_addr_equal(entry, hash, &pkt->peer_addr, pkt->peer_addr_len);
  if (cache_hit && !is_stun) {
    entry->last_seen_ns = pkt->recv_ts_ns;
    return entry->worker_index;
  }

  uint32_t selected = cache_hit ? entry->worker_index : fallback;
  if (s->routing_table && s->ice_creds && is_stun) {
    char client_ufrag[32];
    if (sfu_stun_extract_client_ufrag(pkt->data, pkt->len, s->ice_creds->ufrag, client_ufrag, sizeof(client_ufrag))) {
      sfu_routing_snapshot_t route;
      if (sfu_routing_table_peek_route(s->routing_table, client_ufrag, &route) && route.room) {
        pthread_mutex_lock(&route.room->lock);
        uint32_t peer_count = route.room->peer_count;
        uint64_t room_id = route.room->room_id;
        pthread_mutex_unlock(&route.room->lock);
        if (peer_count <= SFU_SMALL_ROOM_AFFINITY_MAX) {
          selected = fnv1a(&room_id, sizeof(room_id)) % s->worker_count;
        } else if (route.has_owner && route.worker_index < s->worker_count) {
          selected = route.worker_index;
        }
      }
    }
  }

  memset(entry, 0, sizeof(*entry));
  entry->addr = pkt->peer_addr;
  entry->addr_len = pkt->peer_addr_len;
  entry->hash = hash;
  entry->worker_index = selected;
  entry->last_seen_ns = pkt->recv_ts_ns;
  entry->valid = true;
  return selected;
}

int sfu_scheduler_init(sfu_scheduler_t *s, int core_id, int fd, sfu_packet_pool_t *pp, sfu_worker_t *workers, uint32_t worker_count,
                       sfu_routing_table_t *routing_table, const sfu_ice_credentials_t *ice_creds, int recv_bgid, uint32_t buf_count, uint32_t buf_size) {
  memset(s, 0, sizeof(*s));
  s->core_id = core_id;
  s->fd = fd;
  s->pp = pp;
  s->workers = workers;
  s->worker_count = worker_count;
  s->routing_table = routing_table;
  s->ice_creds = ice_creds;

  if (sfu_ring_init(&s->recv_ring, fd, SFU_DISPATCH_SQ_ENTRIES, SFU_DISPATCH_CQ_ENTRIES, buf_count, buf_size, recv_bgid, true) != 0) {
    SFU_LOG_ERROR("scheduler: failed to init recv ring");
    return -1;
  }

  return 0;
}

void sfu_scheduler_destroy(sfu_scheduler_t *s) { sfu_ring_destroy(&s->recv_ring); }

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
  uint32_t worker_idx = scheduler_select_worker(s, pkt, h);

  if (!sfu_spsc_ring_push(&s->workers[worker_idx].inbox, pkt)) {
    sfu_metric_inc("worker_inbox_full");
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

  uint32_t idle_sleep_us = SFU_DISPATCH_IDLE_SLEEP_MIN_US;

  while (!sfu_shutdown_requested()) {
    unsigned reaped = sfu_ring_reap(&s->recv_ring, SFU_DISPATCH_REAP_BATCH, s->pp, NULL, on_recv, NULL, &ctx);

    unsigned returned = 0;
    unsigned backend_work = 0;
    for (uint32_t i = 0; i < s->worker_count; i++) {
      returned += sfu_ring_drain_kernel_buffer_returns(&s->recv_ring, &s->workers[i].release_to_dispatcher, SFU_DISPATCH_REAP_BATCH);
      backend_work += sfu_ring_backend_service(&s->recv_ring, &s->workers[i].send_ring, 1, SFU_DISPATCH_REAP_BATCH);
    }

    if (reaped == 0 && returned == 0 && backend_work == 0) {
      usleep(idle_sleep_us);
      if (idle_sleep_us < SFU_DISPATCH_IDLE_SLEEP_MAX_US) {
        idle_sleep_us = idle_sleep_us * 2;
        if (idle_sleep_us > SFU_DISPATCH_IDLE_SLEEP_MAX_US) {
          idle_sleep_us = SFU_DISPATCH_IDLE_SLEEP_MAX_US;
        }
      }
    } else {
      idle_sleep_us = SFU_DISPATCH_IDLE_SLEEP_MIN_US;
    }
  }

  for (unsigned pass = 0; pass < 2500; pass++) {
    bool pending = false;
    unsigned work = 0;
    for (uint32_t i = 0; i < s->worker_count; i++) {
      pending |= sfu_ring_outstanding_sends(&s->workers[i].send_ring) > 0;
      work += sfu_ring_drain_kernel_buffer_returns(&s->recv_ring, &s->workers[i].release_to_dispatcher, SFU_DISPATCH_REAP_BATCH);
      work += sfu_ring_backend_service(&s->recv_ring, &s->workers[i].send_ring, 1, SFU_DISPATCH_REAP_BATCH);
    }
    if (!pending) {
      break;
    }
    if (!work) {
      usleep(SFU_DISPATCH_IDLE_SLEEP_MIN_US);
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
