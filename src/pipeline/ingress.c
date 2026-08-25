#include "pipeline/ingress.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>

#include "congestion/bandwidth_allocator.h"
#include "congestion/gcc.h"
#include "congestion/pacer.h"
#include "congestion/twcc_feedback.h"
#include "congestion/twcc_history.h"
#include "congestion/twcc_parser.h"
#include "media/svc/layer_scheduler.h"
#include "media/svc/svc_descriptor.h"
#include "memory/packet_pool.h"
#include "net/net.h"
#include "peer/session.h"
#include "pipeline/keyframe.h"
#include "protocol/signaling/signaling.h"
#include "room/room_media_graph.h"
#include "rtcp/rtcp_compound.h"
#include "rtcp/rtcp_fb.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtp_packet.h"
#include "rtp/rtx.h"
#include "rtp/rtx_build.h"
#include "runtime/fanout.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"
#include "util/metrics.h"
#include "util/netbytes.h"

#include <inttypes.h>
#include <stdatomic.h>

#define SFU_INGRESS_NACK_REQUEST_CAP 48
#define SFU_INGRESS_TWCC_BATCH_CAP 256

static uint32_t allocation_for_stream(const sfu_bandwidth_allocation_t *allocation, uint32_t publisher_peer_id,
                                      sfu_bandwidth_stream_kind_t kind) {
  for (size_t i = 0; i < allocation->stream_count; i++) {
    if (allocation->streams[i].publisher_peer_id == publisher_peer_id && allocation->streams[i].kind == kind) {
      return allocation->streams[i].allocated_bps;
    }
  }
  return 0;
}

static uint32_t source_demand_bps(uint32_t allocation_bps) {
  return allocation_bps >= SFU_BANDWIDTH_SOURCE_ADMISSION_BPS ? allocation_bps : 0;
}

static bool allocation_contains_stream(const sfu_bandwidth_allocation_t *allocation, uint64_t stream_key) {
  for (size_t i = 0; i < allocation->stream_count; i++) {
    uint8_t source = allocation->streams[i].kind == SFU_BANDWIDTH_STREAM_SCREEN ? SFU_MEDIA_SCREEN : SFU_MEDIA_VIDEO;
    uint64_t allocated_key = ((uint64_t)allocation->streams[i].publisher_peer_id << 8) | source;
    if (allocated_key == stream_key) {
      return true;
    }
  }
  return false;
}

void sfu_svc_update_layers(sfu_peer_session_t *session, uint32_t bitrate_bps) {
  if (!session) {
    return;
  }
  sfu_pacer_set_rate(&session->egress.pacer, bitrate_bps, (int64_t)sfu_now_us());

  sfu_bandwidth_stream_input_t inputs[SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS];
  size_t input_count = 0;
  sfu_receiver_snapshot_t *snapshot = sfu_session_subscriptions_acquire(session);
  if (snapshot) {
    sfu_receiver_snapshot_iter_t iter;
    sfu_receiver_snapshot_iter_init(&iter, snapshot);
    uint32_t remote_slot = 0;
    const sfu_receiver_entry_t *entry;
    while (input_count < SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS &&
           (entry = sfu_receiver_snapshot_iter_next(&iter, &remote_slot)) != NULL) {
      if (entry->publisher_peer_id == 0) {
        continue;
      }
      if (entry->has_screen && entry->screen_active && input_count < SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS) {
        inputs[input_count++] = (sfu_bandwidth_stream_input_t){
            .publisher_peer_id = entry->publisher_peer_id,
            .remote_slot = remote_slot,
            .assignment_generation = entry->assignment_generation,
            .kind = SFU_BANDWIDTH_STREAM_SCREEN,
            .active = true,
        };
      }
      if (entry->has_video && entry->video_active && input_count < SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS) {
        inputs[input_count++] = (sfu_bandwidth_stream_input_t){
            .publisher_peer_id = entry->publisher_peer_id,
            .remote_slot = remote_slot,
            .assignment_generation = entry->assignment_generation,
            .kind = SFU_BANDWIDTH_STREAM_CAMERA,
            .active = true,
        };
      }
    }
  }

  sfu_bandwidth_allocation_t allocation;
  sfu_bandwidth_allocate(inputs, input_count, bitrate_bps, &allocation);
  uint64_t now_us = sfu_now_us();
  session->egress.diag.last_allocation_us = now_us;
  session->egress.diag.allocation_streams = (uint32_t)allocation.stream_count;
  session->egress.diag.allocation_pool_bps = allocation.video_pool_bps > UINT32_MAX ? UINT32_MAX : (uint32_t)allocation.video_pool_bps;
  session->egress.diag.allocation_reserve_bps = allocation.reserve_bps > UINT32_MAX ? UINT32_MAX : (uint32_t)allocation.reserve_bps;
  session->egress.diag.allocation_allocated_bps = allocation.allocated_bps > UINT32_MAX ? UINT32_MAX : (uint32_t)allocation.allocated_bps;
  session->egress.diag.allocation_unallocated_bps = allocation.unallocated_bps > UINT32_MAX ? UINT32_MAX : (uint32_t)allocation.unallocated_bps;
  session->egress.diag.remb_contribution_bps = session->egress.diag.allocation_allocated_bps;
  if (snapshot) {
    sfu_receiver_snapshot_iter_t contribution_iter;
    sfu_receiver_snapshot_iter_init(&contribution_iter, snapshot);
    uint32_t remote_slot = 0;
    const sfu_receiver_entry_t *entry;
    while ((entry = sfu_receiver_snapshot_iter_next(&contribution_iter, &remote_slot)) != NULL) {
      uint32_t camera_bps = allocation_for_stream(&allocation, entry->publisher_peer_id, SFU_BANDWIDTH_STREAM_CAMERA);
      uint32_t screen_bps = allocation_for_stream(&allocation, entry->publisher_peer_id, SFU_BANDWIDTH_STREAM_SCREEN);
      sfu_session_write_remb_contribution(session, remote_slot, entry->assignment_generation, source_demand_bps(camera_bps),
                                          source_demand_bps(screen_bps), now_us);
    }
  }
  if (session->egress.schedulers) {
    for (size_t i = 0; i < allocation.stream_count; i++) {
      const sfu_bandwidth_stream_allocation_t *stream = &allocation.streams[i];
      sfu_media_kind_t source = stream->kind == SFU_BANDWIDTH_STREAM_SCREEN ? SFU_MEDIA_SCREEN : SFU_MEDIA_VIDEO;
      sfu_layer_scheduler_t *sched = sfu_layer_scheduler_for_stream(session, stream->publisher_peer_id, source);
      sfu_layer_scheduler_set_bitrate(sched, stream->allocated_bps);
    }
    for (uint32_t i = 0; i < SFU_LAYER_SCHEDULER_CAP; i++) {
      sfu_layer_scheduler_slot_t *slot = &session->egress.schedulers[i];
      if (slot->publisher_id != 0 && !allocation_contains_stream(&allocation, slot->publisher_id)) {
        sfu_layer_scheduler_set_bitrate(&slot->sched, 0);
      }
    }
  }
  sfu_subscriptions_snapshot_release(snapshot);
}

static sfu_peer_session_t *find_publisher_by_media_ssrc(sfu_peer_session_t *subscriber, uint32_t media_ssrc, sfu_media_kind_t *out_source) {
  sfu_room_t *room = subscriber->room;
  if (!room) {
    return NULL;
  }

  pthread_mutex_lock(&room->lock);
  sfu_peer_session_t *result = NULL;
  for (uint32_t i = 0; i < room->peer_capacity; i++) {
    sfu_peer_session_t *publisher = room->peers[i];
    if (!publisher || publisher == subscriber) {
      continue;
    }
    sfu_media_snapshot_t pub_msnap = sfu_session_load_media(publisher);
    sfu_media_kind_t source = SFU_MEDIA_VIDEO;
    bool media_matches = pub_msnap.video_ssrc == media_ssrc || pub_msnap.video_rtx_ssrc == media_ssrc;
    if (!media_matches && (pub_msnap.screen_ssrc == media_ssrc || pub_msnap.screen_rtx_ssrc == media_ssrc)) {
      media_matches = true;
      source = SFU_MEDIA_SCREEN;
    }
    if (!media_matches) {
      continue;
    }
    sfu_fanout_bundle_t *bundle = sfu_session_fanout_acquire(publisher);
    sfu_fanout_iter_t iter;
    sfu_fanout_iter_init(&iter, bundle, source);
    const sfu_fanout_route_t *route;
    while ((route = sfu_fanout_iter_next(&iter, NULL)) != NULL) {
      if (route->subscriber == subscriber) {
        atomic_fetch_add_explicit(&publisher->refcount, 1, memory_order_relaxed);
        if (out_source) *out_source = source;
        result = publisher;
        break;
      }
    }
    sfu_fanout_bundle_release(bundle);
    if (result) {
      break;
    }
  }
  pthread_mutex_unlock(&room->lock);
  return result;
}

static void request_source_keyframe(sfu_worker_t *w, sfu_peer_session_t *feedback_session, uint32_t media_ssrc) {
  sfu_media_kind_t source = SFU_MEDIA_VIDEO;
  sfu_peer_session_t *publisher = find_publisher_by_media_ssrc(feedback_session, media_ssrc, &source);
  if (!publisher) {
    sfu_metric_inc("rtcp_kf_unresolved");
    publisher = feedback_session;
    atomic_fetch_add_explicit(&publisher->refcount, 1, memory_order_relaxed);
  }
  sfu_worker_request_keyframe_throttled_for_source(w, publisher, source);
  sfu_session_release(publisher);
}

static void handle_twcc_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  if (!sfu_session_video_runtime_ready(sender_session)) {
    return;
  }
  sfu_twcc_parser_t parser;
  if (sfu_twcc_parser_init(&parser, view->member, view->member_len, sender_session->egress.twcc_last_feedback_ref_us) != 0) {
    sfu_metric_inc("rtcp_twcc_bad");
    return;
  }

  if (parser.packet_status_count > SFU_INGRESS_TWCC_BATCH_CAP) {
    sfu_metric_inc("rtcp_twcc_bad");
    return;
  }

  sfu_twcc_status_t batch[SFU_INGRESS_TWCC_BATCH_CAP];
  size_t batch_count = 0;
  while (batch_count < parser.packet_status_count && sfu_twcc_parser_next_status(&parser, &batch[batch_count])) {
    batch_count++;
  }
  if (parser.failed || parser.packets_processed != parser.packet_status_count || batch_count != parser.packet_status_count) {
    sfu_metric_inc("rtcp_twcc_bad");
    return;
  }

  uint32_t estimated_bps = sender_session->egress.gcc_ctx ? sender_session->egress.gcc_ctx->aimd.current_bitrate_bps : 0;
  uint32_t fresh_lost = 0;
  uint32_t fresh_total = 0;

  for (size_t i = 0; i < batch_count; i++) {
    if (!sender_session->egress.twcc_history) {
      break;
    }
    if (batch[i].status == TWCC_STATUS_NOT_RECEIVED) {
      if (sfu_twcc_history_report_loss_once(sender_session->egress.twcc_history, batch[i].sequence_number)) {
        fresh_lost++;
        fresh_total++;
      }
      continue;
    }

    gcc_packet_info_t item = {.sequence_number = batch[i].sequence_number, .receive_time_us = batch[i].receive_time_us};
    bool was_loss_reported = false;
    if (sfu_twcc_history_consume_received(sender_session->egress.twcc_history, item.sequence_number, &item, &was_loss_reported)) {
      if (sender_session->egress.gcc_ctx) {
        estimated_bps = gcc_bwe_process_twcc_packet(sender_session->egress.gcc_ctx, &item);
      }
      if (!was_loss_reported) {
        fresh_total++;
      }
    }
  }

  if (parser.current_time_us > sender_session->egress.twcc_last_feedback_ref_us) {
    sender_session->egress.twcc_last_feedback_ref_us = parser.current_time_us;
  }

  if (sender_session->egress.gcc_ctx && fresh_lost > 0) {
    gcc_bwe_report_loss(sender_session->egress.gcc_ctx, fresh_lost, fresh_total);
    estimated_bps = sender_session->egress.gcc_ctx->aimd.current_bitrate_bps;
  }

  sender_session->egress.diag.last_twcc_us = sfu_now_us();
  sender_session->egress.diag.latest_twcc_lost = fresh_lost;
  sender_session->egress.diag.latest_twcc_total = fresh_total;
  if (sender_session->egress.gcc_ctx) {
    sender_session->egress.diag.latest_gcc_bps = sender_session->egress.gcc_ctx->aimd.current_bitrate_bps;
    sender_session->egress.diag.latest_ack_bps = sender_session->egress.gcc_ctx->aimd.ack_bitrate_bps;
    sender_session->egress.diag.latest_overuse = (uint8_t)sender_session->egress.gcc_ctx->trendline.usage_state;
  }
  sfu_metric_inc("congestion_twcc_feedback");
  sfu_metric_add("congestion_twcc_lost", fresh_lost);

  if (estimated_bps > 0) {
    sfu_svc_update_layers(sender_session, estimated_bps);
  }
}

static void handle_nack_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_nack_parser_t nack_parser;
  if (!sfu_nack_parser_init(&nack_parser, view->member, view->member_len)) {
    sfu_metric_inc("rtcp_nack_bad");
    return;
  }

  if (!sender_session->egress.rtx_cache) {
    return;
  }

  uint32_t nack_media_ssrc = sfu_nack_parser_media_ssrc(&nack_parser);
  uint32_t nack_generation = atomic_load_explicit(&sender_session->egress.generation, memory_order_acquire);

  uint16_t requested[SFU_INGRESS_NACK_REQUEST_CAP];
  uint32_t requested_count = 0;
  bool capped = false;

  uint16_t lost_seq;
  bool unrecoverable_loss = false;
  while (sfu_nack_parser_next(&nack_parser, &lost_seq)) {
    bool duplicate = false;
    for (uint32_t i = 0; i < requested_count; i++) {
      if (requested[i] == lost_seq) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    if (requested_count >= SFU_INGRESS_NACK_REQUEST_CAP) {
      capped = true;
      break;
    }
    requested[requested_count++] = lost_seq;
    sender_session->egress.diag.nack_requests++;
    sfu_metric_inc("congestion_nack_requested");

    uint8_t orig_pkt[SFU_MAX_PAYLOAD_SIZE];
    uint32_t orig_len = 0;
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_pt = 0;

    if (!sfu_rtx_cache_get_stream(sender_session->egress.rtx_cache, lost_seq, orig_pkt, &orig_len, &rtx_ssrc, &rtx_pt, nack_media_ssrc, nack_generation)) {
      sender_session->egress.diag.cache_misses++;
      sfu_metric_inc("congestion_rtx_cache_miss");
      unrecoverable_loss = true;
      continue;
    }
    sender_session->egress.diag.cache_hits++;
    sfu_metric_inc("congestion_rtx_cache_hit");

    if (!sfu_pacer_rtx_allow(&sender_session->egress.pacer, orig_len + 2, (int64_t)sfu_now_us())) {
      sfu_metric_inc("rtx_dropped_budget");
      continue;
    }

    sfu_packet_t *rtx_enc = sfu_packet_pool_alloc(w->pp);
    if (!rtx_enc) {
      continue;
    }

    uint16_t source_rtx_seq = atomic_fetch_add_explicit(&sender_session->egress.rtx_cache->next_rtx_seq, 1, memory_order_relaxed);
    uint16_t subscriber_rtx_seq = 0;
    pthread_mutex_lock(&sender_session->crypto_lock);
    bool translated = sfu_rtp_seq_translate(&sender_session->cold->rtp_seq_translator, rtx_ssrc, source_rtx_seq, &subscriber_rtx_seq);
    pthread_mutex_unlock(&sender_session->crypto_lock);
    if (!translated) {
      sfu_metric_inc("rtx_seq_translate_fail");
      sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, rtx_enc);
      continue;
    }
    size_t rtx_built_len = 0;
    if (!sfu_rtx_build(orig_pkt, orig_len, rtx_pt, subscriber_rtx_seq, rtx_ssrc, rtx_enc->data, rtx_enc->cap, &rtx_built_len)) {
      sfu_metric_inc("rtx_build_fail");
      sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, rtx_enc);
      continue;
    }

    int64_t send_time_us = (int64_t)sfu_now_us();
    if (sender_session->media.twcc_send_extmap_id != 0) {
      uint16_t twcc_seq = atomic_fetch_add_explicit(&sender_session->egress.next_twcc_seq, 1, memory_order_relaxed);
      size_t rewritten_len = rtx_built_len;
      if (sfu_rtp_ext_write_twcc(rtx_enc->data, rtx_built_len, rtx_enc->cap, sender_session->media.twcc_send_extmap_id, twcc_seq, &rewritten_len)) {
        rtx_built_len = rewritten_len;
        if (sender_session->egress.twcc_history) {
          sfu_twcc_history_record(sender_session->egress.twcc_history, twcc_seq, send_time_us, (uint32_t)rtx_built_len);
        }
      } else {
        sfu_metric_inc("twcc_write_fail");
      }
    }

    int rtx_enc_len = (int)rtx_built_len;
    pthread_mutex_lock(&sender_session->crypto_lock);
    srtp_err_status_t protect_status = sfu_srtp_protect_rtp_status(&sender_session->srtp, rtx_enc->data, &rtx_enc_len, rtx_enc->cap);
    pthread_mutex_unlock(&sender_session->crypto_lock);
    if (protect_status == srtp_err_status_ok) {
      rtx_enc->len = (uint32_t)rtx_enc_len;
      if (sfu_net_send(w->send_net, rtx_enc, (const struct sockaddr *)&sender_session->cold->addr, sender_session->cold->addr_len) == 0) {
        sender_session->egress.diag.rtx_sent++;
        sfu_metric_inc("congestion_rtx_sent");
      }
    } else {
      if (protect_status == srtp_err_status_replay_old) {
        sfu_metric_inc("rtx_protect_replay_old");
      } else if (protect_status == srtp_err_status_replay_fail) {
        sfu_metric_inc("rtx_protect_replay_fail");
      }
      sfu_metric_inc("rtx_protect_fail");
    }
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, rtx_enc);
  }

  if (capped) {
    sfu_metric_inc("rtcp_nack_dropped");
  }

  if (unrecoverable_loss) {
    // Cache miss: ask the source publisher of the lost stream for a keyframe.
    request_source_keyframe(w, sender_session, nack_media_ssrc);
  }
}

static void handle_pli_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_rtcp_pli pli;
  if (!sfu_rtcp_parse_pli(view, &pli)) {
    sfu_metric_inc("rtcp_pli_bad");
    return;
  }

  sender_session->egress.diag.pli_received++;
  sfu_metric_inc("congestion_pli_received");
  request_source_keyframe(w, sender_session, pli.media_ssrc);
}

static void handle_fir_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_rtcp_fir fir;
  if (!sfu_rtcp_parse_fir(view, &fir)) {
    sfu_metric_inc("rtcp_pli_bad");
    return;
  }
  SFU_LOG_DEBUG("worker %u: RTCP FIR from peer %u (media_ssrc=%u, %zu entries) ignored", w->worker_index, sender_session->peer_id, fir.media_ssrc,
                fir.entry_count);
}

static void handle_rtcp(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_packet_t *pkt) {
  sfu_rtcp_compound_iter iter;
  sfu_rtcp_compound_iter_init(&iter, pkt->data, pkt->len);

  sfu_rtcp_member_view view;
  for (;;) {
    sfu_rtcp_compound_result rc = sfu_rtcp_compound_iter_next(&iter, &view);
    if (rc == SFU_RTCP_COMPOUND_END) {
      break;
    }
    if (rc == SFU_RTCP_COMPOUND_MALFORMED) {
      sfu_metric_inc("rtcp_compound_malformed");
      break;
    }

    switch (view.pt) {
      case 205:  // RTPFB
        if (view.fmt_count == 15) {
          handle_twcc_member(w, sender_session, &view);
        } else if (view.fmt_count == 1) {
          handle_nack_member(w, sender_session, &view);
        } else {
          sfu_metric_inc("rtcp_member_unknown");
        }
        break;
      case 206:  // PSFB
        if (view.fmt_count == 1) {
          handle_pli_member(w, sender_session, &view);
        } else if (view.fmt_count == 4) {
          handle_fir_member(w, sender_session, &view);
        } else {
          sfu_metric_inc("rtcp_member_unknown");
        }
        break;
      default:
        // SR/RR/SDES/BYE and anything else: no worker action this phase.
        sfu_metric_inc("rtcp_member_unknown");
        break;
    }
  }

  sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
}

typedef enum sfu_svc_parse_status {
  SFU_SVC_NOT_PRESENT = 0,
  SFU_SVC_PARSE_OK,
  SFU_SVC_PARSE_MALFORMED,
} sfu_svc_parse_status_t;

static sfu_media_kind_t classify_media_source(const sfu_media_snapshot_t *snap, const sfu_rtp_packet_t *rtp) {
  if (rtp->extension && snap->mid_recv_extmap_id != 0) {
    char mid[8];
    if (sfu_rtp_ext_read_mid(rtp->extension_profile, rtp->extension_data, rtp->extension_length, snap->mid_recv_extmap_id, mid, sizeof(mid))) {
      if (strcmp(mid, "2") == 0) {
        return SFU_MEDIA_SCREEN;
      }
      if (strcmp(mid, "1") == 0) {
        return SFU_MEDIA_VIDEO;
      }
      if (strcmp(mid, "0") == 0) {
        return SFU_MEDIA_AUDIO;
      }
    }
  }
  if (rtp->ssrc != 0) {
    if (rtp->ssrc == snap->screen_ssrc || rtp->ssrc == snap->screen_rtx_ssrc) {
      return SFU_MEDIA_SCREEN;
    }
    if (rtp->ssrc == snap->video_ssrc || rtp->ssrc == snap->video_rtx_ssrc) {
      return SFU_MEDIA_VIDEO;
    }
    if (rtp->ssrc == snap->audio_ssrc) {
      return SFU_MEDIA_AUDIO;
    }
  }
  bool camera_pt = rtp->payload_type == snap->video_pt || rtp->payload_type == snap->video_rtx_pt;
  bool screen_pt = rtp->payload_type == snap->screen_pt || rtp->payload_type == snap->screen_rtx_pt;
  if (screen_pt && !camera_pt) {
    return SFU_MEDIA_SCREEN;
  }
  if (camera_pt) {
    return SFU_MEDIA_VIDEO;
  }
  return SFU_MEDIA_AUDIO;
}

static sfu_svc_parse_status_t extract_svc_metadata(sfu_peer_session_t *sender_session, sfu_ingress_media_t *m) {
  m->has_svc = false;
  m->is_keyframe = false;

  sfu_media_snapshot_t msnap = sfu_session_load_media(sender_session);
  uint8_t video_pt = m->source == SFU_MEDIA_SCREEN ? msnap.screen_pt : msnap.video_pt;
  sfu_video_codec_t codec = (sfu_video_codec_t)(m->source == SFU_MEDIA_SCREEN ? msnap.screen_codec : msnap.video_codec);

  if (m->source == SFU_MEDIA_AUDIO || m->rtp.payload_type != video_pt || codec != SFU_VIDEO_CODEC_VP9) {
    return SFU_SVC_NOT_PRESENT;
  }

  if (sfu_svc_parse_descriptor(codec, m->rtp.payload, m->rtp.payload_len, &m->svc) != 0) {
#ifdef SFU_DIAG_LOG
    static _Atomic uint32_t malformed_logs;
    uint32_t n = atomic_fetch_add_explicit(&malformed_logs, 1, memory_order_relaxed);
    if (n == 0 || (n & 127u) == 0) {
      SFU_LOG_WARN("[VP9-DBG] malformed n=%u pub=%u source=%u ssrc=%u seq=%u ts=%u pt=%u payload=%zu", n + 1, sender_session->peer_id,
                   (unsigned)m->source, m->rtp.ssrc, m->rtp.sequence_number, m->rtp.timestamp, m->rtp.payload_type, m->rtp.payload_len);
    }
#endif
    return SFU_SVC_PARSE_MALFORMED;
  }

  m->svc.rtp_timestamp = m->rtp.timestamp;
  m->has_svc = true;
  m->is_keyframe = sfu_svc_descriptor_is_keyframe(&m->svc);
#ifdef SFU_DIAG_LOG
  if (m->source == SFU_MEDIA_SCREEN && m->svc.b_bit != 0) {
    static _Atomic uint32_t picture_logs;
    uint32_t n = atomic_fetch_add_explicit(&picture_logs, 1, memory_order_relaxed);
    if (m->is_keyframe || n == 0 || (n & 127u) == 0) {
      SFU_LOG_INFO("[VP9-DBG] picture n=%u pub=%u ssrc=%u seq=%u ts=%u payload=%zu sid=%u tid=%u b=%u e=%u p=%u u=%u d=%u kf=%u",
                   n + 1, sender_session->peer_id, m->rtp.ssrc, m->rtp.sequence_number, m->rtp.timestamp, m->rtp.payload_len, m->svc.sid,
                   m->svc.tid, m->svc.b_bit, m->svc.e_bit, m->svc.p_bit, m->svc.u_bit, m->svc.d_bit, m->is_keyframe);
    }
  }
#endif
  return SFU_SVC_PARSE_OK;
}

void sfu_ingress_process(sfu_worker_t *w, sfu_packet_t *pkt) {
  sfu_peer_session_t *sender_session = sfu_session_table_find(w->sessions, &pkt->peer_addr, pkt->peer_addr_len);

  if (!sender_session) {
    char ip[INET6_ADDRSTRLEN] = "unknown";
    uint16_t port = 0;
    if (pkt->peer_addr.ss_family == AF_INET) {
      const struct sockaddr_in *s4 = (const struct sockaddr_in *)&pkt->peer_addr;
      inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof(ip));
      port = ntohs(s4->sin_port);
    } else if (pkt->peer_addr.ss_family == AF_INET6) {
      const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&pkt->peer_addr;
      inet_ntop(AF_INET6, &s6->sin6_addr, ip, sizeof(ip));
      port = ntohs(s6->sin6_port);
    }
    SFU_LOG_DEBUG("worker %u: [INGRESS DROP] RTP from unknown peer %s:%u pkt_len=%u", w->worker_index, ip, port, pkt->len);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }
  if (sender_session->state != SFU_SESSION_ESTABLISHED) {
    SFU_LOG_WARN("worker %u: [INGRESS DROP] RTP from unestablished session (state=%d)! pkt_len=%u", w->worker_index, sender_session->state, pkt->len);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  pthread_mutex_lock(&sender_session->ingress_lock);
  if (sfu_session_owner_worker(sender_session) != w->worker_index || !sfu_session_accepts_work(sender_session)) {
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  bool is_rtcp = sfu_rtp_is_rtcp(pkt->data, pkt->len);

  if (!is_rtcp && pkt->len >= 12 && atomic_load_explicit(&sender_session->media.is_mute, memory_order_acquire)) {
    uint32_t raw_ssrc = sfu_read_be32(pkt->data + 8);
    sfu_media_snapshot_t mute_msnap = sfu_session_load_media(sender_session);
    if (raw_ssrc == mute_msnap.audio_ssrc && mute_msnap.audio_ssrc != 0) {
      sfu_metric_inc("muted_audio_drop");
      pthread_mutex_unlock(&sender_session->ingress_lock);
      sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
      sfu_session_release(sender_session);
      return;
    }
  }

  int plain_len = (int)pkt->len;
  srtp_err_status_t unprotect_status;
  bool used_previous_generation = false;

#ifdef SFU_DIAG_LOG
  bool has_inbound_ctx = false;
  bool has_previous_ctx = false;
  int64_t previous_remaining_ms = 0;
#endif

  pthread_mutex_lock(&sender_session->crypto_lock);
  uint64_t now_ms = sfu_now_ms();
  if (sender_session->previous_srtp.inbound && now_ms >= sender_session->previous_srtp_expires_ms) {
    sfu_srtp_ctx_destroy(&sender_session->previous_srtp);
    sender_session->previous_srtp_expires_ms = 0;
  }

  uint8_t ciphertext[SFU_MAX_PAYLOAD_SIZE];
  bool have_ciphertext = pkt->len <= sizeof(ciphertext);
  if (have_ciphertext) {
    memcpy(ciphertext, pkt->data, pkt->len);
  }
  bool can_retry_previous = sender_session->previous_srtp.inbound && have_ciphertext;

  unprotect_status = is_rtcp ? sfu_srtp_unprotect_rtcp_status(&sender_session->srtp, pkt->data, &plain_len)
                             : sfu_srtp_unprotect_rtp_status(&sender_session->srtp, pkt->data, &plain_len);
  if (unprotect_status == srtp_err_status_auth_fail && can_retry_previous) {
    memcpy(pkt->data, ciphertext, pkt->len);
    plain_len = (int)pkt->len;
    srtp_err_status_t previous_status = is_rtcp ? sfu_srtp_unprotect_rtcp_status(&sender_session->previous_srtp, pkt->data, &plain_len)
                                                : sfu_srtp_unprotect_rtp_status(&sender_session->previous_srtp, pkt->data, &plain_len);
    if (previous_status == srtp_err_status_ok) {
      unprotect_status = srtp_err_status_ok;
      used_previous_generation = true;
    }
  }

  bool roc_recovered = false;
#ifdef SFU_DIAG_LOG
  uint32_t diag_roc_before = 0;
  uint32_t diag_roc_after = 0;
  bool diag_roc_valid = false;
  bool diag_roc_retry = false;
#endif
  if ((unprotect_status == srtp_err_status_replay_old || unprotect_status == srtp_err_status_replay_fail) && !is_rtcp && have_ciphertext && pkt->len >= 12) {
    uint32_t raw_ssrc = sfu_read_be32(pkt->data + 8);
    uint32_t ct_ssrc = sfu_read_be32(ciphertext + 8);
    if (raw_ssrc == 0) {
      raw_ssrc = ct_ssrc;
    }
    uint32_t roc = 0;
    if (sfu_srtp_get_roc(&sender_session->srtp, raw_ssrc, &roc) == srtp_err_status_ok) {
#ifdef SFU_DIAG_LOG
      diag_roc_before = roc;
      diag_roc_valid = true;
#endif
      uint32_t next_roc = roc + 1;
      if (sfu_srtp_set_roc(&sender_session->srtp, raw_ssrc, next_roc) == srtp_err_status_ok) {
        memcpy(pkt->data, ciphertext, pkt->len);
        plain_len = (int)pkt->len;
        srtp_err_status_t retry_status = sfu_srtp_unprotect_rtp_status(&sender_session->srtp, pkt->data, &plain_len);
#ifdef SFU_DIAG_LOG
        diag_roc_retry = true;
        diag_roc_after = next_roc;
#endif
        if (retry_status == srtp_err_status_ok) {
          unprotect_status = srtp_err_status_ok;
          roc_recovered = true;
        } else {
          sfu_srtp_set_roc(&sender_session->srtp, raw_ssrc, roc);
          memcpy(pkt->data, ciphertext, pkt->len);
          plain_len = (int)pkt->len;
#ifdef SFU_DIAG_LOG
          diag_roc_after = roc;
#endif
        }
      }
    } else if (ct_ssrc != 0 && ct_ssrc != raw_ssrc) {
      if (sfu_srtp_get_roc(&sender_session->srtp, ct_ssrc, &roc) == srtp_err_status_ok) {
#ifdef SFU_DIAG_LOG
        diag_roc_before = roc;
        diag_roc_valid = true;
#endif
        uint32_t next_roc = roc + 1;
        if (sfu_srtp_set_roc(&sender_session->srtp, ct_ssrc, next_roc) == srtp_err_status_ok) {
          memcpy(pkt->data, ciphertext, pkt->len);
          plain_len = (int)pkt->len;
          srtp_err_status_t retry_status = sfu_srtp_unprotect_rtp_status(&sender_session->srtp, pkt->data, &plain_len);
#ifdef SFU_DIAG_LOG
          diag_roc_retry = true;
          diag_roc_after = next_roc;
#endif
          if (retry_status == srtp_err_status_ok) {
            unprotect_status = srtp_err_status_ok;
            roc_recovered = true;
          } else {
            sfu_srtp_set_roc(&sender_session->srtp, ct_ssrc, roc);
            memcpy(pkt->data, ciphertext, pkt->len);
            plain_len = (int)pkt->len;
#ifdef SFU_DIAG_LOG
            diag_roc_after = roc;
#endif
          }
        }
      }
    }
  }

#ifdef SFU_DIAG_LOG
  has_inbound_ctx = sender_session->srtp.inbound != NULL;
  has_previous_ctx = sender_session->previous_srtp.inbound != NULL;
  if (has_previous_ctx) {
    uint64_t expiry = sender_session->previous_srtp_expires_ms;
    previous_remaining_ms = (now_ms >= expiry) ? 0 : (int64_t)(expiry - now_ms);
  }
#endif

  pthread_mutex_unlock(&sender_session->crypto_lock);

  if (used_previous_generation) {
    sfu_metric_inc("ingress_unprotect_previous_generation");
  }
  if (roc_recovered) {
    sfu_metric_inc("ingress_roc_recovered");
#ifdef SFU_DIAG_LOG
    {
      uint32_t raw_ssrc = 0;
      uint16_t raw_seq = 0;
      if (!is_rtcp && pkt->len >= 12) {
        raw_seq = sfu_read_be16(pkt->data + 2);
        raw_ssrc = sfu_read_be32(pkt->data + 8);
      }
      SFU_LOG_INFO("worker %u: [INGRESS ROC] recovered peer=%u ufrag=%s ssrc=%" PRIu32 " seq=%" PRIu16 " roc=%" PRIu32 "->%" PRIu32, w->worker_index,
                   sender_session->peer_id, sender_session->cold->ufrag, raw_ssrc, raw_seq, diag_roc_before, diag_roc_after);
    }
#endif
  }

  if (unprotect_status != srtp_err_status_ok) {
    sfu_metric_inc(is_rtcp ? "ingress_unprotect_fail_rtcp" : "ingress_unprotect_fail_rtp");
    if (unprotect_status == srtp_err_status_auth_fail) {
      sfu_metric_inc("ingress_unprotect_auth_fail");
    } else if (unprotect_status == srtp_err_status_replay_fail) {
      sfu_metric_inc("ingress_unprotect_replay_fail");
    } else if (unprotect_status == srtp_err_status_replay_old) {
      sfu_metric_inc("ingress_unprotect_replay_old");
    } else if (unprotect_status == srtp_err_status_no_ctx) {
      sfu_metric_inc("ingress_unprotect_no_ctx");
    } else {
      sfu_metric_inc("ingress_unprotect_other");
    }

#ifdef SFU_DIAG_LOG
    {
      uint32_t raw_ssrc = 0;
      uint16_t raw_pt = 0;
      uint16_t raw_seq = 0;
      if (!is_rtcp && pkt->len >= 12) {
        raw_ssrc = sfu_read_be32(ciphertext + 8);
        raw_seq = sfu_read_be16(ciphertext + 2);
        raw_pt = (uint16_t)(ciphertext[1] & 0x7Fu);
      }
      sfu_media_snapshot_t dump_msnap = sfu_session_load_media(sender_session);
      bool dump_is_audience = atomic_load_explicit(&sender_session->is_audience, memory_order_acquire);
      bool dump_ptt_active = atomic_load_explicit(&sender_session->media.ptt_active, memory_order_acquire);
      bool dump_vis = atomic_load_explicit(&sender_session->media.visible, memory_order_acquire);
      bool dump_is_mute = atomic_load_explicit(&sender_session->media.is_mute, memory_order_acquire);
      bool dump_audio_send = atomic_load_explicit(&sender_session->media.audio_send_negotiated, memory_order_acquire);
      bool dump_pending_dtls = sender_session->cold->pending_dtls_active;
      bool dump_client_random_valid = sender_session->cold->active_client_random_valid;
      uint32_t dump_transport_gen = sender_session->cold->transport_generation;
      unsigned long dump_profile = sender_session->cold->dtls.srtp_profile_id;
      int64_t dump_user_id = sender_session->user_id;
      char dump_addr_str[64] = "unknown";
      if (sender_session->cold->addr.ss_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)&sender_session->cold->addr;
        inet_ntop(AF_INET, &s4->sin_addr, dump_addr_str, sizeof(dump_addr_str));
      } else if (sender_session->cold->addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&sender_session->cold->addr;
        inet_ntop(AF_INET6, &s6->sin6_addr, dump_addr_str, sizeof(dump_addr_str));
      }
      SFU_LOG_WARN("worker %u: [INGRESS DROP] SRTP unprotect failed peer=%u user_id=%" PRId64 " ufrag=%s status=%d (%s) rtcp=%d len=%u generation=%u owner=%u",
                   w->worker_index, sender_session->peer_id, dump_user_id, sender_session->cold->ufrag, (int)unprotect_status,
                   sfu_srtp_status_name(unprotect_status), is_rtcp, pkt->len, dump_transport_gen, sfu_session_owner_worker(sender_session));
      SFU_LOG_INFO("worker %u: [INGRESS DUMP] peer=%u user_id=%" PRId64 " ufrag=%s addr=%s raw_ssrc=%" PRIu32 " raw_pt=%" PRIu16 " raw_seq=%" PRIu16
                   " srtp_inbound=%d srtp_prev_inbound=%d prev_rem_ms=%" PRId64
                   " srtp_profile=0x%lx pending_dtls=%d client_random_valid=%d"
                   " transport_gen=%" PRIu32
                   " owner=%u is_audience=%d ptt_active=%d audio_send_negotiated=%d visible=%d is_mute=%d"
                   " audio_ssrc=%" PRIu32
                   " audio_active=%d state=%d verified=%d"
                   " roc_valid=%d roc_retry=%d roc_before=%" PRIu32 " roc_after=%" PRIu32,
                   w->worker_index, sender_session->peer_id, dump_user_id, sender_session->cold->ufrag, dump_addr_str, raw_ssrc, raw_pt, raw_seq,
                   has_inbound_ctx ? 1 : 0, has_previous_ctx ? 1 : 0, previous_remaining_ms, dump_profile, dump_pending_dtls ? 1 : 0,
                   dump_client_random_valid ? 1 : 0, dump_transport_gen, sfu_session_owner_worker(sender_session), dump_is_audience ? 1 : 0,
                   dump_ptt_active ? 1 : 0, dump_audio_send ? 1 : 0, dump_vis ? 1 : 0, dump_is_mute ? 1 : 0, dump_msnap.audio_ssrc,
                   dump_msnap.audio_active ? 1 : 0, (int)sender_session->state, sender_session->cold->dtls.established ? 1 : 0, diag_roc_valid ? 1 : 0,
                   diag_roc_retry ? 1 : 0, diag_roc_before, diag_roc_after);
    }
#endif

    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }
  pkt->len = (uint32_t)plain_len;

  if (is_rtcp) {
    handle_rtcp(w, sender_session, pkt);
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_session_release(sender_session);
    return;
  }

  sfu_ingress_media_t m;
  m.pkt = pkt;
  if (!sfu_rtp_packet_parse(pkt->data, pkt->len, &m.rtp)) {
    SFU_LOG_DEBUG("worker %u: [INGRESS DROP] malformed RTP header (len=%u)", w->worker_index, pkt->len);
    sfu_metric_inc("rtp_parse_fail");
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  sfu_media_snapshot_t pt_msnap = sfu_session_load_media(sender_session);
  uint8_t in_pt = m.rtp.payload_type;
  m.source = classify_media_source(&pt_msnap, &m.rtp);
  m.is_audio = m.source == SFU_MEDIA_AUDIO;
  bool is_audience = atomic_load_explicit(&sender_session->is_audience, memory_order_acquire);
  bool ptt_active = atomic_load_explicit(&sender_session->media.ptt_active, memory_order_acquire);
  if (is_audience && (!m.is_audio || !ptt_active)) {
    sfu_metric_inc(m.is_audio ? "ptt_inactive_audio_drop" : "audience_rtp_drop");
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }
  if (m.is_audio && atomic_load_explicit(&sender_session->media.is_mute, memory_order_acquire)) {
    sfu_metric_inc("muted_audio_drop");
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  uint8_t twcc_recv_extmap_id = pt_msnap.twcc_recv_extmap_id;
  if (sfu_session_video_runtime_ready(sender_session) && sender_session->egress.twcc_recv && twcc_recv_extmap_id != 0 && m.rtp.extension) {
    uint16_t twcc_seq = 0;
    if (sfu_rtp_ext_read_twcc(m.rtp.extension_profile, m.rtp.extension_data, m.rtp.extension_length, twcc_recv_extmap_id, &twcc_seq)) {
      int64_t arrival_us;
      if (pkt->recv_ts_ns != 0) {
        arrival_us = (int64_t)(pkt->recv_ts_ns / 1000ULL);
        sender_session->egress.twcc_recv->have_kernel_clock = true;
        sender_session->egress.twcc_recv->last_arrival_us = arrival_us;
      } else if (sender_session->egress.twcc_recv->have_kernel_clock && sender_session->egress.twcc_recv->last_arrival_us != 0) {
        arrival_us = sender_session->egress.twcc_recv->last_arrival_us + 1;
        sender_session->egress.twcc_recv->last_arrival_us = arrival_us;
      } else {
        arrival_us = (int64_t)sfu_now_us();
        sender_session->egress.twcc_recv->last_arrival_us = arrival_us;
      }
      sfu_twcc_recv_tracker_record(sender_session->egress.twcc_recv, twcc_seq, arrival_us);
    }
  }

  bool send_negotiated = m.source == SFU_MEDIA_VIDEO    ? atomic_load_explicit(&sender_session->media.video_send_negotiated, memory_order_acquire)
                         : m.source == SFU_MEDIA_SCREEN ? atomic_load_explicit(&sender_session->media.screen_send_negotiated, memory_order_acquire)
                                                        : atomic_load_explicit(&sender_session->media.audio_send_negotiated, memory_order_acquire);
  bool learned = false;
  if (!send_negotiated) {
    sfu_metric_inc("unnegotiated_rtp_drop");
#ifdef SFU_DIAG_LOG
    static _Atomic uint32_t unnegotiated_drop_logs;
    uint32_t n = atomic_fetch_add_explicit(&unnegotiated_drop_logs, 1, memory_order_relaxed);
    if (n == 0 || (n & 127u) == 0) {
      SFU_LOG_WARN("ingress: unnegotiated_rtp_drop n=%u peer=%u ufrag=%s source=%d pt=%u ssrc=%" PRIu32
                   " audio_neg=%d video_neg=%d screen_neg=%d audience=%d",
                   n + 1, sender_session->peer_id, sender_session->cold->ufrag, (int)m.source, in_pt, m.rtp.ssrc,
                   atomic_load_explicit(&sender_session->media.audio_send_negotiated, memory_order_acquire) ? 1 : 0,
                   atomic_load_explicit(&sender_session->media.video_send_negotiated, memory_order_acquire) ? 1 : 0,
                   atomic_load_explicit(&sender_session->media.screen_send_negotiated, memory_order_acquire) ? 1 : 0,
                   is_audience ? 1 : 0);
    }
#endif
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }
  if (m.source == SFU_MEDIA_VIDEO && !atomic_load_explicit(&sender_session->media.camera_enabled, memory_order_acquire)) {
    sfu_metric_inc("camera_disabled_rtp_drop");
#ifdef SFU_DIAG_LOG
    static _Atomic uint32_t camera_disabled_logs;
    uint32_t n = atomic_fetch_add_explicit(&camera_disabled_logs, 1, memory_order_relaxed);
    if (n == 0 || (n & 127u) == 0) {
      SFU_LOG_WARN("ingress: camera_disabled_rtp_drop n=%u peer=%u ufrag=%s ssrc=%" PRIu32, n + 1, sender_session->peer_id, sender_session->cold->ufrag,
                   m.rtp.ssrc);
    }
#endif
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }
  if (m.source == SFU_MEDIA_SCREEN && !atomic_load_explicit(&sender_session->media.screen_enabled, memory_order_acquire)) {
    sfu_metric_inc("screen_disabled_rtp_drop");
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }
  if (is_audience && !atomic_load_explicit(&sender_session->media.ptt_active, memory_order_acquire)) {
    sfu_metric_inc("ptt_inactive_audio_drop");
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  sfu_transceiver_t *source = m.source == SFU_MEDIA_SCREEN  ? &sender_session->media.screen
                              : m.source == SFU_MEDIA_VIDEO ? &sender_session->media.uplink_video
                                                            : &sender_session->media.uplink_audio;
  uint32_t known_ssrc = m.source == SFU_MEDIA_SCREEN ? pt_msnap.screen_ssrc : m.source == SFU_MEDIA_VIDEO ? pt_msnap.video_ssrc : pt_msnap.audio_ssrc;
  uint32_t known_rtx_ssrc = m.source == SFU_MEDIA_SCREEN ? pt_msnap.screen_rtx_ssrc : pt_msnap.video_rtx_ssrc;
  uint8_t rtx_pt = m.source == SFU_MEDIA_SCREEN ? pt_msnap.screen_rtx_pt : pt_msnap.video_rtx_pt;
  bool active = m.source == SFU_MEDIA_SCREEN ? pt_msnap.screen_active : m.source == SFU_MEDIA_VIDEO ? pt_msnap.video_active : pt_msnap.audio_active;
  bool is_rtx = !m.is_audio && (in_pt == rtx_pt || (known_rtx_ssrc != 0 && m.rtp.ssrc == known_rtx_ssrc));
  bool need_learn = (m.rtp.ssrc != 0 && (is_rtx ? known_rtx_ssrc : known_ssrc) != m.rtp.ssrc) || !active;

  if (need_learn) {
    pthread_mutex_lock(&sender_session->media.lock);
    if (!is_rtx && !m.is_audio && source->rtx_ssrc != 0 && m.rtp.ssrc == source->rtx_ssrc) {
      is_rtx = true;
    }
    if (is_rtx) {
      if (m.rtp.ssrc != 0 && source->rtx_ssrc != m.rtp.ssrc) {
#ifdef SFU_DIAG_LOG
        SFU_LOG_INFO("ingress: SSRC learn RTX peer=%u ufrag=%s source=%d old_rtx=%" PRIu32 " new=%" PRIu32 " pt=%u rtx_pt=%u active=%d",
                     sender_session->peer_id, sender_session->cold->ufrag, (int)m.source, source->rtx_ssrc, m.rtp.ssrc, in_pt, rtx_pt, active ? 1 : 0);
#endif
        source->rtx_ssrc = m.rtp.ssrc;
        learned = true;
      }
    } else if (m.rtp.ssrc != 0 && source->ssrc != m.rtp.ssrc) {
      if (source->rtx_ssrc != 0 && m.rtp.ssrc == source->rtx_ssrc) {
#ifdef SFU_DIAG_LOG
        SFU_LOG_INFO("ingress: skip media SSRC learn of RTX ssrc peer=%u ufrag=%s source=%d ssrc=%" PRIu32 " pt=%u rtx_pt=%u", sender_session->peer_id,
                     sender_session->cold->ufrag, (int)m.source, m.rtp.ssrc, in_pt, rtx_pt);
#endif
      } else {
#ifdef SFU_DIAG_LOG
        SFU_LOG_WARN("ingress: SSRC learn media peer=%u ufrag=%s source=%d old_ssrc=%" PRIu32 " new=%" PRIu32
                     " pt=%u rtx_pt=%u is_rtx=%d known_ssrc=%" PRIu32 " known_rtx=%" PRIu32 " active=%d",
                     sender_session->peer_id, sender_session->cold->ufrag, (int)m.source, source->ssrc, m.rtp.ssrc, in_pt, rtx_pt, is_rtx ? 1 : 0, known_ssrc,
                     known_rtx_ssrc, active ? 1 : 0);
#endif
        source->ssrc = m.rtp.ssrc;
        learned = true;
      }
    }
    if (!is_rtx && m.source == SFU_MEDIA_VIDEO && !atomic_load_explicit(&sender_session->media.camera_rtp_observed, memory_order_acquire)) {
      atomic_store_explicit(&sender_session->media.camera_rtp_observed, true, memory_order_release);
      learned = true;
    } else if (!is_rtx && m.source == SFU_MEDIA_SCREEN && !atomic_load_explicit(&sender_session->media.screen_rtp_observed, memory_order_acquire)) {
      atomic_store_explicit(&sender_session->media.screen_rtp_observed, true, memory_order_release);
      learned = true;
    }
    if (m.is_audio) {
      if (!source->active) {
        source->active = true;
        learned = true;
      }
    } else if (sfu_session_recompute_video_activity_locked(sender_session)) {
      learned = true;
    }
    if (learned) {
      sfu_session_publish_media(sender_session);
    }
    pthread_mutex_unlock(&sender_session->media.lock);
  }
  if (learned) {
    atomic_store_explicit(&sender_session->media.uplink_ssrc_dirty, true, memory_order_release);
  }
  sfu_svc_parse_status_t svc_status = extract_svc_metadata(sender_session, &m);
  if (svc_status == SFU_SVC_PARSE_MALFORMED) {
    sfu_metric_inc("vp9_descriptor_parse_fail");
    sfu_worker_request_keyframe_throttled_for_source(w, sender_session, m.source);
    pthread_mutex_unlock(&sender_session->ingress_lock);
    sfu_net_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  sfu_router_forward(w, sender_session, &m);

  if (atomic_exchange_explicit(&sender_session->media.uplink_ssrc_dirty, false, memory_order_acq_rel) && sender_session->room) {
    pthread_mutex_lock(&sender_session->media.lock);
    uint32_t learned_audio_ssrc = sender_session->media.uplink_audio.ssrc;
    uint32_t learned_video_ssrc = sender_session->media.uplink_video.ssrc;
    uint32_t learned_screen_ssrc = sender_session->media.screen.ssrc;
    pthread_mutex_unlock(&sender_session->media.lock);
#ifdef SFU_DIAG_LOG
    SFU_LOG_INFO("worker %u: learned uplink SSRCs from RTP peer=%u ufrag=%s source=%d is_rtx=%d pt=%u rtx_pt=%u"
                 " (audio=%u camera=%u screen=%u audio_active=%d video_active=%d screen_active=%d); refreshing forwarding",
                 w->worker_index, sender_session->peer_id, sender_session->cold->ufrag, (int)m.source, is_rtx ? 1 : 0, in_pt, rtx_pt, learned_audio_ssrc,
                 learned_video_ssrc, learned_screen_ssrc, sender_session->media.uplink_audio.active ? 1 : 0, sender_session->media.uplink_video.active ? 1 : 0,
                 sender_session->media.screen.active ? 1 : 0);
#else
    SFU_LOG_INFO("worker %u: learned uplink SSRCs from RTP for ufrag=%s (audio=%u camera=%u screen=%u); refreshing forwarding", w->worker_index,
                 sender_session->cold->ufrag, learned_audio_ssrc, learned_video_ssrc, learned_screen_ssrc);
#endif
    room_refresh_peer_streams((sfu_room_t *)sender_session->room, sender_session);
    if (m.source == SFU_MEDIA_VIDEO || m.source == SFU_MEDIA_SCREEN) {
      sfu_signaling_notify_media_state(sender_session);
    }
  }
  pthread_mutex_unlock(&sender_session->ingress_lock);
  sfu_session_release(sender_session);
}
