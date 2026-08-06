#include "pipeline/ingress.h"

#include <pthread.h>
#include <string.h>

#include "congestion/gcc.h"
#include "congestion/twcc_history.h"
#include "congestion/twcc_parser.h"
#include "media/svc/svc_descriptor.h"
#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/keyframe.h"
#include "protocol/signaling/sdp.h"
#include "rtcp/rtcp_compound.h"
#include "rtcp/rtcp_fb.h"
#include "rtp/rtp_packet.h"
#include "rtp/rtx.h"
#include "rtp/rtx_build.h"
#include "runtime/scheduler.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"
#include "util/metrics.h"

#define SFU_INGRESS_NACK_REQUEST_CAP 48
#define SFU_INGRESS_TWCC_BATCH_CAP 256

void sfu_svc_update_layers(sfu_peer_session_t *session, uint32_t bitrate_bps) {
  sfu_pacer_set_rate(&session->pacer, bitrate_bps, (int64_t)sfu_now_us());
  if (!session->schedulers) {
    return;
  }
  for (uint32_t i = 0; i < SFU_SESSION_SCHEDULER_CAP; i++) {
    sfu_session_scheduler_slot_t *slot = &session->schedulers[i];
    if (slot->publisher_id != 0) {
      sfu_subscriber_scheduler_set_bitrate(&slot->sched, bitrate_bps);
    }
  }
}

static sfu_peer_session_t *find_publisher_by_media_ssrc(sfu_peer_session_t *subscriber, uint32_t media_ssrc) {
  sfu_room_t *room = subscriber->room;
  if (!room) {
    return NULL;
  }

  pthread_mutex_lock(&room->lock);
  sfu_peer_session_t *result = NULL;
  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *publisher = room->peers[i];
    if (publisher == subscriber) {
      continue;
    }
    if (publisher->uplink_video.ssrc != media_ssrc && publisher->uplink_video.rtx_ssrc != media_ssrc) {
      continue;
    }
    sfu_receiver_snapshot_t *snap = sfu_session_receivers_acquire(publisher);
    if (!snap) {
      continue;
    }
    for (uint32_t j = 0; j < snap->count; j++) {
      const sfu_receiver_entry_t *e = &snap->entries[j];
      if (e->subscriber == subscriber && e->has_video) {
        atomic_fetch_add_explicit(&publisher->refcount, 1, memory_order_relaxed);
        result = publisher;
        break;
      }
    }
    sfu_receiver_snapshot_release(snap);
    if (result) {
      break;
    }
  }
  pthread_mutex_unlock(&room->lock);
  return result;
}

static void request_source_keyframe(sfu_worker_t *w, sfu_peer_session_t *feedback_session, uint32_t media_ssrc) {
  sfu_peer_session_t *publisher = find_publisher_by_media_ssrc(feedback_session, media_ssrc);
  if (!publisher) {
    sfu_metric_inc("rtcp_kf_unresolved");
    publisher = feedback_session;
    atomic_fetch_add_explicit(&publisher->refcount, 1, memory_order_relaxed);
  }
  sfu_worker_request_keyframe_throttled(w, publisher);
  sfu_session_release(publisher);
}

static void handle_twcc_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_twcc_parser_t parser;
  if (sfu_twcc_parser_init(&parser, view->member, view->member_len, sender_session->twcc_last_feedback_ref_us) != 0) {
    sfu_metric_inc("rtcp_twcc_bad");
    return;
  }

  gcc_packet_info_t batch[SFU_INGRESS_TWCC_BATCH_CAP];
  size_t batch_count = 0;
  gcc_packet_info_t item;
  while (batch_count < SFU_INGRESS_TWCC_BATCH_CAP && sfu_twcc_parser_next(&parser, &item)) {
    batch[batch_count++] = item;
  }
  if (parser.packets_processed < parser.packet_status_count) {
    sfu_metric_inc("rtcp_twcc_bad");
    return;
  }

  uint32_t estimated_bps = sender_session->gcc_ctx ? sender_session->gcc_ctx->aimd.current_bitrate_bps : 0;

  for (size_t i = 0; i < batch_count; i++) {
    if (sender_session->twcc_history && sfu_twcc_history_lookup(sender_session->twcc_history, batch[i].sequence_number, &batch[i])) {
      if (sender_session->gcc_ctx) {
        estimated_bps = gcc_bwe_process_twcc_packet(sender_session->gcc_ctx, &batch[i]);
      }
    }
  }

  if (batch_count > 0) {
    sender_session->twcc_last_feedback_ref_us = batch[batch_count - 1].receive_time_us;
  }

  if (sender_session->gcc_ctx && parser.packets_lost > 0) {
    gcc_bwe_report_loss(sender_session->gcc_ctx, parser.packets_lost, parser.packet_status_count);
    estimated_bps = sender_session->gcc_ctx->aimd.current_bitrate_bps;
  }

  if (estimated_bps > 0) {
    sfu_svc_update_layers(sender_session, estimated_bps);
  }
  (void)w;
}

static void handle_nack_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_nack_parser_t nack_parser;
  if (!sfu_nack_parser_init(&nack_parser, view->member, view->member_len)) {
    sfu_metric_inc("rtcp_nack_bad");
    return;
  }

  if (!sender_session->rtx_cache) {
    return;
  }

  uint32_t nack_media_ssrc = sfu_nack_parser_media_ssrc(&nack_parser);
  uint32_t nack_generation = atomic_load_explicit(&sender_session->egress_generation, memory_order_acquire);

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

    if (!sfu_pacer_rtx_allow(&sender_session->pacer, 1200 /* conservative MTU estimate */, (int64_t)sfu_now_us())) {
      sfu_metric_inc("rtx_dropped_budget");
      continue;
    }

    uint8_t orig_pkt[SFU_MAX_PAYLOAD_SIZE];
    uint32_t orig_len = 0;
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_pt = 0;

    if (sfu_rtx_cache_get_stream(sender_session->rtx_cache, lost_seq, orig_pkt, &orig_len, &rtx_ssrc, &rtx_pt, nack_media_ssrc, nack_generation)) {
      sfu_packet_t *rtx_enc = sfu_packet_pool_alloc(w->pp);
      if (!rtx_enc) {
        continue;
      }

      uint16_t next_rtx_seq = __atomic_fetch_add(&sender_session->rtx_cache->next_rtx_seq, 1, __ATOMIC_RELAXED);
      size_t rtx_built_len = 0;
      if (!sfu_rtx_build(orig_pkt, orig_len, rtx_pt, next_rtx_seq, rtx_ssrc, rtx_enc->data, rtx_enc->cap, &rtx_built_len)) {
        sfu_metric_inc("rtx_build_fail");
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtx_enc);
        continue;
      }

      int rtx_enc_len = (int)rtx_built_len;
      if (sfu_srtp_protect_rtp(&sender_session->srtp, rtx_enc->data, &rtx_enc_len, rtx_enc->cap)) {
        rtx_enc->len = (uint32_t)rtx_enc_len;
        sfu_ring_queue_send_zc(&w->send_ring, rtx_enc, (const struct sockaddr *)&sender_session->cold->addr, sender_session->cold->addr_len);
      }
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtx_enc);
    } else {
      unrecoverable_loss = true;
    }
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

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
}

static void extract_svc_metadata(sfu_peer_session_t *sender_session, sfu_ingress_media_t *m) {
  m->has_svc = false;
  m->is_keyframe = false;

  if (m->is_audio || m->rtp.payload_type != sender_session->uplink_video.payload_type) {
    return;
  }

  sfu_video_codec_t codec = sfu_video_codec_from_pt(m->rtp.payload_type);
  if (codec == SFU_VIDEO_CODEC_NONE) {
    return;
  }

  if (sender_session->uplink_video.ssrc == 0 && m->rtp.ssrc != 0) {
    sender_session->uplink_video.ssrc = m->rtp.ssrc;
  }

  if (sfu_svc_parse_descriptor(codec, m->rtp.payload, m->rtp.payload_len, &m->svc) == 0) {
    m->has_svc = true;
    m->is_keyframe = sfu_svc_descriptor_is_keyframe(&m->svc);
  }
}

void sfu_ingress_process(sfu_worker_t *w, sfu_packet_t *pkt) {
  sfu_peer_session_t *sender_session = sfu_session_table_find(w->sessions, &pkt->peer_addr, pkt->peer_addr_len);

  if (!sender_session) {
    SFU_LOG_WARN("worker %u: [INGRESS DROP] RTP from unknown peer! pkt_len=%u", w->worker_index, pkt->len);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }
  if (sender_session->state != SFU_SESSION_ESTABLISHED) {
    SFU_LOG_WARN("worker %u: [INGRESS DROP] RTP from unestablished session (state=%d)! pkt_len=%u", w->worker_index, sender_session->state, pkt->len);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  bool is_rtcp = sfu_rtp_is_rtcp(pkt->data, pkt->len);
  int plain_len = (int)pkt->len;
  bool unprotected =
      is_rtcp ? sfu_srtp_unprotect_rtcp(&sender_session->srtp, pkt->data, &plain_len) : sfu_srtp_unprotect_rtp(&sender_session->srtp, pkt->data, &plain_len);

  if (!unprotected) {
    SFU_LOG_WARN("worker %u: [INGRESS DROP] SRTP unprotect FAILED (is_rtcp=%d, len=%u). Key mismatch or corrupted packet!", w->worker_index, is_rtcp, pkt->len);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }
  pkt->len = (uint32_t)plain_len;

  if (is_rtcp) {
    handle_rtcp(w, sender_session, pkt);
    sfu_session_release(sender_session);
    return;
  }

  sfu_ingress_media_t m;
  m.pkt = pkt;
  if (!sfu_rtp_packet_parse(pkt->data, pkt->len, &m.rtp)) {
    SFU_LOG_DEBUG("worker %u: [INGRESS DROP] malformed RTP header (len=%u)", w->worker_index, pkt->len);
    sfu_metric_inc("rtp_parse_fail");
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  m.is_audio = sender_session->uplink_audio.active && m.rtp.payload_type == sender_session->uplink_audio.payload_type;
  extract_svc_metadata(sender_session, &m);

  sfu_router_forward(w, sender_session, &m);
  sfu_session_release(sender_session);
}
