#include "pipeline/paced_send.h"

#include <string.h>

#include "congestion/pacer.h"
#include "congestion/twcc_history.h"
#include "memory/worker_packet_arena.h"
#include "peer/session.h"
#include "rtp/rtx.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "util/alloc.h"
#include "util/metrics.h"

#define SFU_PACED_SEND_CAPACITY 512u

void sfu_paced_send_init(sfu_paced_send_t *q) {
  if (q) {
    memset(q, 0, sizeof(*q));
  }
}

void sfu_paced_send_destroy(sfu_paced_send_t *q) {
  if (!q) {
    return;
  }
  if (q->entries) {
    for (uint32_t i = 0, index = q->head; i < q->count; i++, index = (index + 1u) % q->capacity) {
      sfu_paced_send_entry_t *e = &q->entries[index];
      sfu_pacer_cancel(e->pacer, &e->reservation);
    }
  }
  SFU_FREE(q->entries);
  memset(q, 0, sizeof(*q));
}

int64_t sfu_paced_send_projected_delay_us(const sfu_paced_send_t *q, int64_t now_us) {
  if (!q || q->count == 0) {
    return 0;
  }
  int64_t serialization = q->next_release_us > now_us ? q->next_release_us - now_us : 0;
  uint64_t scans = ((uint64_t)q->ready_count + SFU_PACED_SEND_MAX_DRAIN_PER_SCAN - 1u) / SFU_PACED_SEND_MAX_DRAIN_PER_SCAN;
  int64_t scan_delay = (int64_t)scans * SFU_PACED_SEND_SCAN_INTERVAL_US;
  return serialization > scan_delay ? serialization : scan_delay;
}

bool sfu_paced_send_admit_frame_packet(sfu_paced_send_t *q, uint32_t rtp_timestamp, bool marker, bool keyframe, bool drop_on_delay, int64_t max_delay_us,
                                       int64_t now_us) {
  if (!q) {
    return false;
  }
  bool new_frame = !q->input_frame_active || q->input_timestamp != rtp_timestamp;
  if (new_frame) {
    if (q->input_frame_active && q->input_timestamp != rtp_timestamp) {
      sfu_paced_send_rollback_input_frame(q);
      q->input_frame_active = false;
      q->drop_input_frame = false;
      sfu_metric_inc("paced_send_incomplete_frame_drop");
    }
    if (q->input_frame_active && now_us >= q->input_frame_started_us) {
      int64_t span = now_us - q->input_frame_started_us;
      if (span > q->max_input_frame_span_us) {
        q->max_input_frame_span_us = span;
      }
      if (span > max_delay_us) {
        q->input_frames_over_delay++;
        sfu_metric_inc("paced_send_input_frame_slow");
      }
    }
    q->input_timestamp = rtp_timestamp;
    q->input_frame_started_us = now_us;
    q->input_frame_base_next_release_us = q->next_release_us;
    q->input_frame_queued_packets = 0;
    q->input_frame_active = true;

    /* Detect motion: if queue has >3 frames ready or this frame will be >2× previous size.
     * Use tighter delay bound during motion to reduce blur during scrolling. */
    bool motion_detected = q->ready_count > 3;
    if (motion_detected) {
      q->motion_frame_count++;
    } else if (q->motion_frame_count > 0) {
      q->motion_frame_count--;
    }
    bool in_motion = q->motion_frame_count > 0;
    int64_t effective_max_delay = in_motion ? SFU_PACED_SEND_SCREEN_MOTION_MAX_DELAY_US : max_delay_us;

    q->drop_input_frame = drop_on_delay && !keyframe && sfu_paced_send_projected_delay_us(q, now_us) >= effective_max_delay;
    if (q->drop_input_frame) {
      q->dropped_delay_frames++;
      sfu_metric_inc("paced_send_delay_frame_drop");
    }
  }
  bool admitted = !q->drop_input_frame;
  if (!admitted) {
    q->dropped_frame_packets++;
    sfu_metric_inc("paced_send_frame_packet_drop");
  }
  if (marker && !admitted) {
    q->input_frame_active = false;
    q->drop_input_frame = false;
  }
  return admitted;
}

void sfu_paced_send_finish_input_frame(sfu_paced_send_t *q) {
  if (!q || !q->input_frame_active) {
    return;
  }
  if (q->input_frame_queued_packets) {
    uint32_t last = q->tail == 0 ? q->capacity - 1u : q->tail - 1u;
    q->entries[last].metadata.frame_end = true;
  }
  q->ready_count += q->input_frame_queued_packets;
  q->input_frame_queued_packets = 0;
  q->input_frame_active = false;
  q->drop_input_frame = false;
}

void sfu_paced_send_reject_input_frame(sfu_paced_send_t *q) {
  if (q && q->input_frame_active) {
    q->drop_input_frame = true;
  }
}

void sfu_paced_send_rollback_input_frame(sfu_paced_send_t *q) {
  if (!q || !q->entries || q->input_frame_queued_packets == 0 || q->input_frame_queued_packets > q->count) {
    return;
  }
  for (uint32_t i = 0; i < q->input_frame_queued_packets; i++) {
    q->tail = q->tail == 0 ? q->capacity - 1u : q->tail - 1u;
    sfu_paced_send_entry_t *e = &q->entries[q->tail];
    if (e->metadata.publisher_peer_id != 0) {
      q->rolled_back_publisher_peer_id = e->metadata.publisher_peer_id;
    }
    sfu_pacer_cancel(e->pacer, &e->reservation);
    memset(e, 0, sizeof(*e));
    q->count--;
  }
  q->next_release_us = q->input_frame_base_next_release_us;
  q->input_frame_queued_packets = 0;
  if (!q->count) {
    q->head = q->tail = 0;
    q->next_release_us = 0;
  }
}

static void rebase_backlog(sfu_paced_send_t *q, int64_t now_us) {
  int64_t base = now_us;
  for (uint32_t i = 0, index = q->head; i < q->count; i++, index = (index + 1u) % q->capacity) {
    q->entries[index].release_at_us = base;
    base += q->entries[index].span_us;
  }
  q->next_release_us = q->count ? base : 0;
  if (q->input_frame_active) {
    int64_t unpublished_span = 0;
    for (uint32_t i = 0, index = q->head; i < q->ready_count; i++, index = (index + 1u) % q->capacity) {
      unpublished_span += q->entries[index].span_us;
    }
    q->input_frame_base_next_release_us = now_us + unpublished_span;
  }
}

void sfu_paced_send_drop_report_init(sfu_paced_send_drop_report_t *report) {
  if (report) {
    memset(report, 0, sizeof(*report));
  }
}

void sfu_paced_send_drop_report_add(sfu_paced_send_drop_report_t *report, uint32_t publisher_peer_id) {
  if (!report || publisher_peer_id == 0) {
    return;
  }
  for (uint32_t i = 0; i < report->publisher_count; i++) {
    if (report->publisher_peer_ids[i] == publisher_peer_id) {
      return;
    }
  }
  if (report->publisher_count >= SFU_PACED_SEND_MAX_DROPPED_PUBLISHERS) {
    report->truncated = true;
    return;
  }
  report->publisher_peer_ids[report->publisher_count++] = publisher_peer_id;
}

bool sfu_paced_send_bound_backlog(sfu_paced_send_t *q, int64_t max_delay_us, int64_t now_us, sfu_paced_send_drop_report_t *report) {
  if (!q || max_delay_us <= 0 || q->input_frame_active) {
    return false;
  }
  while (q->ready_count && sfu_paced_send_projected_delay_us(q, now_us) >= max_delay_us) {
    if (q->entries[q->head].metadata.is_keyframe) {
      break;
    }
    bool frame_end = false;
    /* One pass over the head frame: count it once and record its publisher so
     * the caller can invalidate that subscriber's layer scheduler. */
    uint32_t frame_publisher = 0;
    while (q->ready_count && !frame_end) {
      sfu_paced_send_entry_t *e = &q->entries[q->head];
      frame_end = e->metadata.frame_end;
      frame_publisher = e->metadata.publisher_peer_id;
      sfu_pacer_cancel(e->pacer, &e->reservation);
      memset(e, 0, sizeof(*e));
      q->head = (q->head + 1u) % q->capacity;
      q->count--;
      q->ready_count--;
    }
    q->dropped_delay_frames++;
    sfu_metric_inc("paced_send_delay_frame_drop");
    if (report) {
      report->frames++;
      sfu_paced_send_drop_report_add(report, frame_publisher);
    }
    rebase_backlog(q, now_us);
  }
  if (!q->count) {
    q->head = q->tail = 0;
  }
  return sfu_paced_send_projected_delay_us(q, now_us) < max_delay_us;
}

bool sfu_paced_send_enqueue(sfu_paced_send_t *q, const uint8_t *data, uint16_t len, const uint8_t *rtx_plaintext, uint16_t rtx_plaintext_len,
                            const struct sockaddr_storage *dst, socklen_t dst_len, uint8_t pacer_class, uint32_t pacing_bps, sfu_pacer_t *pacer,
                            sfu_pacer_reservation_t *reservation, const sfu_paced_send_metadata_t *metadata, int64_t now_us, int64_t *release_at_us) {
  if (!q || !data || !dst || !metadata || !reservation || !reservation->active || len == 0 || len > SFU_PACED_SEND_MAX_PAYLOAD ||
      rtx_plaintext_len > SFU_PACED_SEND_MAX_PAYLOAD || (rtx_plaintext_len && !rtx_plaintext)) {
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
  uint32_t min_rate = (metadata && metadata->is_keyframe) ? SFU_PACED_SEND_KEYFRAME_MIN_BPS : SFU_PACED_SEND_MIN_BPS;
  uint32_t rate = pacing_bps < min_rate ? min_rate : pacing_bps;
  int64_t span = ((int64_t)len * 8LL * 1000000LL + rate - 1) / rate;
  int64_t base = q->next_release_us > now_us ? q->next_release_us : now_us;
  sfu_paced_send_entry_t *e = &q->entries[q->tail];
  memset(e, 0, sizeof(*e));
  e->release_at_us = base;
  e->enqueued_at_us = now_us;
  e->span_us = span;
  e->dst = *dst;
  e->dst_len = dst_len;
  e->len = len;
  e->rtx_plaintext_len = rtx_plaintext_len;
  e->pacer_class = pacer_class;
  e->pacer = pacer;
  e->reservation = *reservation;
  reservation->active = false;
  e->metadata = *metadata;
  memcpy(e->data, data, len);
  if (rtx_plaintext_len) {
    memcpy(e->rtx_plaintext, rtx_plaintext, rtx_plaintext_len);
  }
  q->tail = (q->tail + 1u) % q->capacity;
  q->count++;
  q->next_release_us = base + span;
  if (q->input_frame_active) {
    q->input_frame_queued_packets++;
  }
  if (q->count > q->high_water) {
    q->high_water = q->count;
  }
  if (release_at_us) {
    *release_at_us = base;
  }
  sfu_metric_inc("paced_send_enqueued");
  return true;
}

static void pop_entry(sfu_paced_send_t *q) {
  q->head = (q->head + 1u) % q->capacity;
  q->count--;
  q->ready_count--;
}

static void cancel_and_pop_entry(sfu_paced_send_t *q) {
  sfu_paced_send_entry_t *e = &q->entries[q->head];
  sfu_pacer_cancel(e->pacer, &e->reservation);
  q->head = (q->head + 1u) % q->capacity;
  q->count--;
  q->ready_count--;
}

static bool entry_valid(const sfu_paced_send_entry_t *e, const sfu_peer_session_t *s) {
  return sfu_session_accepts_work(s) && sfu_session_owner_value(s) == e->metadata.owner_value &&
         sfu_session_remote_slot_authorized(s, e->metadata.remote_slot, e->metadata.assignment_generation) &&
         atomic_load_explicit(&s->cold->transport_generation, memory_order_acquire) == e->metadata.transport_generation &&
         atomic_load_explicit(&s->cold->address_generation, memory_order_acquire) == e->metadata.address_generation && s->cold->addr_len == e->dst_len &&
         memcmp(&s->cold->addr, &e->dst, e->dst_len) == 0;
}

bool sfu_paced_send_drain(sfu_paced_send_t *q, sfu_worker_t *w, sfu_peer_session_t *s, int64_t now_us, uint32_t *remaining) {
  if (!q || !w || !s || !remaining || *remaining == 0 || !q->entries || q->count == 0) {
    return false;
  }
  uint32_t limit = *remaining;
  bool did_work = false;
  uint32_t processed = 0, sent = 0;
  q->drain_invocations++;
  while (q->ready_count && processed < limit) {
    sfu_paced_send_entry_t *e = &q->entries[q->head];
    if (!entry_valid(e, s)) {
      cancel_and_pop_entry(q);
      q->dropped_stale++;
      processed++;
      did_work = true;
      continue;
    }
    if (e->release_at_us > now_us) {
      break;
    }
    sfu_packet_t *out = sfu_worker_packet_arena_alloc(&w->output_arena);
    if (!out) {
      out = sfu_packet_pool_alloc(w->pp);
    }
    if (!out || e->len > out->cap) {
      if (out) {
        sfu_worker_release_packet(w, out);
      }
      break;
    }
    memcpy(out->data, e->data, e->len);
    out->len = e->len;
    if (sfu_net_send(w->send_net, out, (const struct sockaddr *)&e->dst, e->dst_len) != 0) {
      sfu_worker_release_packet(w, out);
      sfu_metric_inc("paced_send_sq_full");
      break;
    }
    sfu_worker_release_packet(w, out);
    int64_t accepted_us = (now_us > 0 && now_us <= 3000000000000000LL) ? now_us : (int64_t)sfu_now_us();
    if (e->metadata.twcc_written && sfu_session_video_runtime_ready(s) && s->egress.twcc_history) {
      sfu_twcc_history_record(s->egress.twcc_history, e->metadata.twcc_seq, accepted_us, e->len);
    }
    if (e->metadata.cache_rtx && sfu_session_video_runtime_ready(s) && s->egress.rtx_cache) {
      sfu_rtx_cache_put_stream(s->egress.rtx_cache, e->metadata.subscriber_seq, e->rtx_plaintext, e->rtx_plaintext_len, e->metadata.rtx_ssrc,
                               e->metadata.rtx_pt, e->metadata.media_ssrc, e->metadata.egress_generation);
    }
    int64_t residence = accepted_us >= e->enqueued_at_us ? accepted_us - e->enqueued_at_us : 0;
    int64_t late = accepted_us >= e->release_at_us ? accepted_us - e->release_at_us : 0;
    if (residence > q->max_enqueue_to_send_us) {
      q->max_enqueue_to_send_us = residence;
    }
    if (late > q->max_release_late_us) {
      q->max_release_late_us = late;
    }
    q->enqueue_to_send_sum_us += residence;
    q->enqueue_to_send_samples++;
    sfu_pacer_commit(e->pacer, &e->reservation);
    pop_entry(q);
    q->sent++;
    processed++;
    sent++;
    did_work = true;
    sfu_metric_inc("paced_send_sent");
  }
  if (sent > q->max_drain_packets) {
    q->max_drain_packets = sent;
  }
  *remaining -= processed;
  if (processed == limit && q->ready_count && q->entries[q->head].release_at_us <= now_us) {
    q->drain_cap_hits++;
    sfu_metric_inc("paced_send_drain_cap_hit");
  }
  if (!q->count) {
    q->head = q->tail = 0;
    q->next_release_us = 0;
  }
  return did_work;
}

void sfu_paced_priority_queue_init(sfu_paced_priority_queue_t *q, bool is_probe_queue, int64_t max_residence_us, uint32_t max_bytes) {
  if (!q) {
    return;
  }
  memset(q, 0, sizeof(*q));
  q->is_probe_queue = is_probe_queue;
  q->max_residence_us = max_residence_us;
  q->max_bytes = max_bytes;
}

void sfu_paced_priority_queue_destroy(sfu_paced_priority_queue_t *q, sfu_pacer_t *pacer_to_refund) {
  if (!q) {
    return;
  }
  if (q->entries) {
    if (pacer_to_refund) {
      for (uint32_t i = 0, index = q->head; i < q->count; i++, index = (index + 1u) % q->capacity) {
        if (q->entries[index].pacer_charged_bytes > 0) {
          sfu_pacer_rtx_refund(pacer_to_refund, q->entries[index].pacer_charged_bytes);
        }
      }
    }
    SFU_FREE(q->entries);
  }
  memset(q, 0, sizeof(*q));
}

void sfu_paced_priority_queue_clear(sfu_paced_priority_queue_t *q, sfu_pacer_t *pacer_to_refund) {
  if (!q) {
    return;
  }
  if (q->entries && pacer_to_refund) {
    for (uint32_t i = 0, index = q->head; i < q->count; i++, index = (index + 1u) % q->capacity) {
      if (q->entries[index].pacer_charged_bytes > 0) {
        sfu_pacer_rtx_refund(pacer_to_refund, q->entries[index].pacer_charged_bytes);
      }
    }
  }
  q->head = 0;
  q->tail = 0;
  q->count = 0;
  q->byte_count = 0;
}

bool sfu_paced_priority_queue_enqueue(sfu_paced_priority_queue_t *q, const uint8_t *data, uint16_t len,
                                      const struct sockaddr_storage *dst, socklen_t dst_len,
                                      uint64_t owner_value, uint32_t transport_generation,
                                      uint32_t address_generation, bool twcc_written, uint16_t twcc_seq,
                                      uint32_t pacer_charged_bytes, int64_t now_us) {
  if (!q || !data || !dst || len == 0 || len > SFU_PACED_SEND_MAX_PAYLOAD || dst_len == 0) {
    return false;
  }
  if (!q->entries) {
    q->entries = SFU_CALLOC(SFU_PACED_PRIORITY_QUEUE_CAPACITY, sizeof(*q->entries));
    if (!q->entries) {
      return false;
    }
    q->capacity = SFU_PACED_PRIORITY_QUEUE_CAPACITY;
  }
  if (q->count >= q->capacity || (q->max_bytes > 0 && q->byte_count + (uint32_t)len > q->max_bytes)) {
    q->dropped_full++;
    if (q->is_probe_queue) {
      sfu_metric_inc("congestion_probe_full_drop");
    } else {
      sfu_metric_inc("congestion_rtx_full_drop");
    }
    return false;
  }

  sfu_paced_priority_entry_t *e = &q->entries[q->tail];
  memset(e, 0, sizeof(*e));
  memcpy(e->data, data, len);
  e->len = len;
  e->dst = *dst;
  e->dst_len = dst_len;
  e->enqueued_at_us = now_us;
  e->owner_value = owner_value;
  e->transport_generation = transport_generation;
  e->address_generation = address_generation;
  e->twcc_written = twcc_written;
  e->twcc_seq = twcc_seq;
  e->pacer_charged_bytes = pacer_charged_bytes;
  e->is_probe = q->is_probe_queue;
  e->probe_cluster_id = 0;

  q->tail = (q->tail + 1u) % q->capacity;
  q->count++;
  q->byte_count += (uint32_t)len;
  q->enqueued++;

  if (q->is_probe_queue) {
    sfu_metric_inc("congestion_probe_queued");
  } else {
    sfu_metric_inc("congestion_rtx_queued");
    sfu_metric_add("congestion_rtx_bytes", len);
  }
  return true;
}

bool sfu_paced_priority_queue_enqueue_probe(sfu_paced_priority_queue_t *q, const uint8_t *data, uint16_t len,
                                            const struct sockaddr_storage *dst, socklen_t dst_len,
                                            uint64_t owner_value, uint32_t transport_generation,
                                            uint32_t address_generation, bool twcc_written, uint16_t twcc_seq,
                                            uint32_t probe_cluster_id, int64_t now_us) {
  if (!q || !data || len == 0 || len > SFU_PACED_SEND_MAX_PAYLOAD || !dst || dst_len == 0) {
    return false;
  }
  if (!q->entries) {
    q->entries = SFU_CALLOC(SFU_PACED_PRIORITY_QUEUE_CAPACITY, sizeof(*q->entries));
    if (!q->entries) {
      return false;
    }
    q->capacity = SFU_PACED_PRIORITY_QUEUE_CAPACITY;
  }
  if (q->count >= q->capacity || (q->max_bytes > 0 && q->byte_count + (uint32_t)len > q->max_bytes)) {
    q->dropped_full++;
    sfu_metric_inc("congestion_probe_full_drop");
    return false;
  }

  sfu_paced_priority_entry_t *e = &q->entries[q->tail];
  memset(e, 0, sizeof(*e));
  memcpy(e->data, data, len);
  e->len = len;
  e->dst = *dst;
  e->dst_len = dst_len;
  e->enqueued_at_us = now_us;
  e->owner_value = owner_value;
  e->transport_generation = transport_generation;
  e->address_generation = address_generation;
  e->twcc_written = twcc_written;
  e->twcc_seq = twcc_seq;
  e->pacer_charged_bytes = 0;
  e->is_probe = true;
  e->probe_cluster_id = probe_cluster_id;

  q->tail = (q->tail + 1u) % q->capacity;
  q->count++;
  q->byte_count += (uint32_t)len;
  q->enqueued++;

  sfu_metric_inc("congestion_probe_queued");
  return true;
}

static bool priority_entry_valid(const sfu_paced_priority_entry_t *e, const sfu_peer_session_t *s) {
  return s && s->cold && sfu_session_accepts_work(s) && sfu_session_owner_value(s) == e->owner_value &&
         atomic_load_explicit(&s->cold->transport_generation, memory_order_acquire) == e->transport_generation &&
         atomic_load_explicit(&s->cold->address_generation, memory_order_acquire) == e->address_generation &&
         s->cold->addr_len == e->dst_len && memcmp(&s->cold->addr, &e->dst, e->dst_len) == 0;
}

bool sfu_paced_priority_queue_drain(sfu_paced_priority_queue_t *q, sfu_worker_t *w, sfu_peer_session_t *s,
                                    int64_t now_us, uint32_t *remaining) {
  if (!q || !w || !s || !remaining || *remaining == 0 || !q->entries || q->count == 0) {
    return false;
  }
  uint32_t limit = *remaining;
  bool did_work = false;
  uint32_t processed = 0, sent = 0;

  while (q->count && processed < limit) {
    sfu_paced_priority_entry_t *e = &q->entries[q->head];
    if (!priority_entry_valid(e, s)) {
      if (e->pacer_charged_bytes > 0) {
        sfu_pacer_rtx_refund(&s->egress.pacer, e->pacer_charged_bytes);
      }
      q->dropped_stale++;
      if (q->is_probe_queue) {
        sfu_metric_inc("congestion_probe_stale_drop");
      } else {
        sfu_metric_inc("congestion_rtx_stale_drop");
      }
      q->byte_count = q->byte_count >= e->len ? q->byte_count - e->len : 0;
      q->head = (q->head + 1u) % q->capacity;
      q->count--;
      processed++;
      did_work = true;
      continue;
    }

    if (q->max_residence_us > 0 && now_us > e->enqueued_at_us && (now_us - e->enqueued_at_us) > q->max_residence_us) {
      if (e->pacer_charged_bytes > 0) {
        sfu_pacer_rtx_refund(&s->egress.pacer, e->pacer_charged_bytes);
      }
      q->dropped_expired++;
      if (q->is_probe_queue) {
        sfu_metric_inc("congestion_probe_expired_drop");
      } else {
        sfu_metric_inc("congestion_rtx_expired_drop");
      }
      q->byte_count = q->byte_count >= e->len ? q->byte_count - e->len : 0;
      q->head = (q->head + 1u) % q->capacity;
      q->count--;
      processed++;
      did_work = true;
      continue;
    }

    sfu_packet_t *out = sfu_worker_packet_arena_alloc(&w->output_arena);
    if (!out) {
      out = sfu_packet_pool_alloc(w->pp);
    }
    if (!out || e->len > out->cap) {
      if (out) {
        sfu_worker_release_packet(w, out);
      }
      break;
    }

    memcpy(out->data, e->data, e->len);
    out->len = e->len;
    if (sfu_net_send(w->send_net, out, (const struct sockaddr *)&e->dst, e->dst_len) != 0) {
      sfu_worker_release_packet(w, out);
      sfu_metric_inc("paced_send_sq_full");
      break;
    }
    sfu_worker_release_packet(w, out);

    int64_t accepted_us = (now_us > 0 && now_us <= 3000000000000000LL) ? now_us : (int64_t)sfu_now_us();
    if (e->twcc_written && sfu_session_video_runtime_ready(s) && s->egress.twcc_history) {
      if (q->is_probe_queue || e->is_probe) {
        sfu_twcc_history_record_probe(s->egress.twcc_history, e->twcc_seq, accepted_us, e->len, e->probe_cluster_id);
      } else {
        sfu_twcc_history_record(s->egress.twcc_history, e->twcc_seq, accepted_us, e->len);
      }
    }

    q->sent++;
    q->bytes_sent += e->len;
    if (q->is_probe_queue) {
      sfu_metric_inc("congestion_probe_sent");
      sfu_metric_add("congestion_probe_bytes", e->len);
    } else {
#ifdef SFU_DIAG_LOG
      s->egress.diag.rtx_sent++;
#endif
      sfu_metric_inc("congestion_rtx_sent");
    }

    q->byte_count = q->byte_count >= e->len ? q->byte_count - e->len : 0;
    q->head = (q->head + 1u) % q->capacity;
    q->count--;
    processed++;
    sent++;
    did_work = true;
  }

  *remaining -= processed;
  if (!q->count) {
    q->head = 0;
    q->tail = 0;
    q->byte_count = 0;
  }
  return did_work;
}

