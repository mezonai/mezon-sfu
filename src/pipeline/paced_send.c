#include "pipeline/paced_send.h"

#include <string.h>

#include "memory/worker_packet_arena.h"
#include "runtime/worker.h"
#include "util/alloc.h"
#include "util/metrics.h"

#define SFU_PACED_SEND_CAPACITY 512u

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

int64_t sfu_paced_send_projected_delay_us(const sfu_paced_send_t *q, int64_t now_us) {
  if (!q || q->count == 0 || q->next_release_us <= now_us) {
    return 0;
  }
  return q->next_release_us - now_us;
}

bool sfu_paced_send_admit_frame_packet(sfu_paced_send_t *q, uint32_t rtp_timestamp, bool marker, bool keyframe, int64_t now_us) {
  if (!q) {
    return false;
  }
  bool new_frame = !q->input_frame_active || q->input_timestamp != rtp_timestamp;
  if (new_frame) {
    if (q->input_frame_active && now_us >= q->input_frame_started_us) {
      int64_t span_us = now_us - q->input_frame_started_us;
      if (span_us > q->max_input_frame_span_us) {
        q->max_input_frame_span_us = span_us;
      }
      if (span_us > SFU_PACED_SEND_MAX_DELAY_US) {
        q->input_frames_over_delay++;
        sfu_metric_inc("paced_send_input_frame_slow");
      }
    }
    q->input_timestamp = rtp_timestamp;
    q->input_frame_started_us = now_us;
    q->input_frame_active = true;
    q->drop_input_frame = !keyframe && sfu_paced_send_projected_delay_us(q, now_us) >= SFU_PACED_SEND_MAX_DELAY_US;
    if (q->drop_input_frame) {
      q->dropped_delay_frames++;
      sfu_metric_inc("paced_send_delay_frame_drop");
      sfu_metric_inc("paced_send_delay_crossing");
    }
  }
  bool admitted = !q->drop_input_frame;
  if (!admitted) {
    q->dropped_frame_packets++;
    sfu_metric_inc("paced_send_frame_packet_drop");
  }
  if (marker) {
    if (now_us >= q->input_frame_started_us) {
      int64_t span_us = now_us - q->input_frame_started_us;
      if (span_us > q->max_input_frame_span_us) {
        q->max_input_frame_span_us = span_us;
      }
      if (span_us > SFU_PACED_SEND_MAX_DELAY_US) {
        q->input_frames_over_delay++;
        sfu_metric_inc("paced_send_input_frame_slow");
      }
    }
    q->input_frame_active = false;
    q->drop_input_frame = false;
  }
  return admitted;
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
    sfu_metric_inc("paced_send_full_drop");
    return false;
  }

  uint32_t bps = pacing_bps;
  if (bps < SFU_PACED_SEND_MIN_BPS) {
    bps = SFU_PACED_SEND_MIN_BPS;
    sfu_metric_inc("paced_send_rate_floor");
  }

  int64_t bits_us = (int64_t)len * 8LL * 1000000LL;
  int64_t span_us = (bits_us + (int64_t)bps - 1LL) / (int64_t)bps;

  int64_t base = q->next_release_us > now_us ? q->next_release_us : now_us;

  sfu_paced_send_entry_t *e = &q->entries[q->tail];
  e->release_at_us = base;
  e->enqueued_at_us = now_us;
  memcpy(&e->dst, dst, sizeof(e->dst));
  e->dst_len = dst_len;
  e->len = len;
  memcpy(e->data, data, len);

  q->tail = (q->tail + 1u) % q->capacity;
  q->count++;
  if (q->count > q->high_water) {
    q->high_water = q->count;
  }
  q->next_release_us = base + span_us;
  sfu_metric_inc("paced_send_enqueued");
  if (release_at_us) {
    *release_at_us = base;
  }
  return true;
}

bool sfu_paced_send_drain(sfu_paced_send_t *q, sfu_worker_t *w, int64_t now_us) {
  if (!q || !w || q->count == 0 || !q->entries) {
    return false;
  }
  bool sent = false;
  uint32_t sent_this_scan = 0;
  q->drain_invocations++;
  while (q->count > 0 && sent_this_scan < SFU_PACED_SEND_MAX_DRAIN_PER_SCAN) {
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
    sent = true;
    sent_this_scan++;
    int64_t enqueue_to_send_us = now_us >= e->enqueued_at_us ? now_us - e->enqueued_at_us : 0;
    int64_t release_late_us = now_us >= e->release_at_us ? now_us - e->release_at_us : 0;
    q->enqueue_to_send_sum_us += (uint64_t)enqueue_to_send_us;
    q->enqueue_to_send_samples++;
    if (enqueue_to_send_us > q->max_enqueue_to_send_us) {
      q->max_enqueue_to_send_us = enqueue_to_send_us;
    }
    if (release_late_us > q->max_release_late_us) {
      q->max_release_late_us = release_late_us;
    }
    if (release_late_us > SFU_PACED_SEND_SCAN_INTERVAL_US) {
      sfu_metric_inc("paced_send_release_late");
    }
    sfu_metric_inc("paced_send_sent");
  }
  if (sent_this_scan > q->max_drain_packets) {
    q->max_drain_packets = sent_this_scan;
  }
  if (sent_this_scan == SFU_PACED_SEND_MAX_DRAIN_PER_SCAN && q->count > 0 && q->entries[q->head].release_at_us <= now_us) {
    q->drain_cap_hits++;
    sfu_metric_inc("paced_send_drain_cap_hit");
  }
  if (q->count == 0) {
    q->head = 0;
    q->tail = 0;
    q->next_release_us = 0;
  }
  return sent;
}
