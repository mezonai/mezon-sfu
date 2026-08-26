#include "pipeline/paced_send.h"

#include <string.h>

#include "memory/worker_packet_arena.h"
#include "runtime/worker.h"
#include "util/alloc.h"
#include "util/metrics.h"

#define SFU_PACED_SEND_CAPACITY 512u
#define SFU_PACED_SEND_MIN_BPS 200000u

void sfu_paced_send_init(sfu_paced_send_t *q) {
  if (!q) {
    return;
  }
  memset(q, 0, sizeof(*q));
}

void sfu_paced_send_destroy(sfu_paced_send_t *q) {
  if (!q) {
    return;
  }
  SFU_FREE(q->entries);
  memset(q, 0, sizeof(*q));
}

bool sfu_paced_send_enqueue(sfu_paced_send_t *q, const uint8_t *data, uint16_t len, const struct sockaddr_storage *dst, socklen_t dst_len, uint32_t pacing_bps,
                            int64_t now_us, int64_t *release_at_us) {
  if (!q || !data || !dst || len == 0 || len > SFU_PACED_SEND_MAX_PAYLOAD) {
    return false;
  }
  if (!q->entries) {
    q->entries = SFU_CALLOC(SFU_PACED_SEND_CAPACITY, sizeof(*q->entries));
    if (!q->entries) {
      return false;
    }
    q->capacity = SFU_PACED_SEND_CAPACITY;
  }
  if (q->count >= q->capacity) {
    q->dropped_full++;
    return false;
  }

  uint32_t bps = pacing_bps < SFU_PACED_SEND_MIN_BPS ? SFU_PACED_SEND_MIN_BPS : pacing_bps;

  int64_t span_us = ((int64_t)len * 8 * 1000000) / (int64_t)bps;

  int64_t base = q->next_release_us > now_us ? q->next_release_us : now_us;

  sfu_paced_send_entry_t *e = &q->entries[q->tail];
  e->release_at_us = base;
  memcpy(&e->dst, dst, sizeof(e->dst));
  e->dst_len = dst_len;
  e->len = len;
  memcpy(e->data, data, len);

  q->tail = (q->tail + 1u) % q->capacity;
  q->count++;
  q->next_release_us = base + span_us;
  if (release_at_us) {
    *release_at_us = base;
  }
  return true;
}

void sfu_paced_send_drain(sfu_paced_send_t *q, sfu_worker_t *w, int64_t now_us) {
  if (!q || !w || q->count == 0 || !q->entries) {
    return;
  }
  while (q->count > 0) {
    sfu_paced_send_entry_t *e = &q->entries[q->head];
    if (e->release_at_us > now_us) {
      break;
    }

    sfu_packet_t *out = sfu_worker_packet_arena_alloc(&w->output_arena);
    if (out && e->len > out->cap) {
      sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, out);
      out = NULL;
    }
    if (!out) {
      out = sfu_packet_pool_alloc(w->pp);
    }
    if (!out || e->len > out->cap) {
      if (out) {
        sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, out);
      }
      break;
    }

    memcpy(out->data, e->data, e->len);
    out->len = e->len;

    int rc = sfu_net_send(w->send_net, out, (const struct sockaddr *)&e->dst, e->dst_len);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, out);
    if (rc != 0) {
      sfu_metric_inc("paced_send_sq_full");
      break;
    }

    q->head = (q->head + 1u) % q->capacity;
    q->count--;
    q->sent++;
  }
}
