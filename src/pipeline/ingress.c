#include "pipeline/ingress.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>

#include "congestion/gcc.h"
#include "congestion/twcc_feedback.h"
#include "congestion/twcc_history.h"
#include "congestion/twcc_parser.h"
#include "media/svc/svc_descriptor.h"
#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/keyframe.h"
#include "protocol/signaling/sdp.h"
#include "protocol/signaling/signaling.h"
#include "room/room_media_graph.h"
#include "rtcp/rtcp_compound.h"
#include "rtcp/rtcp_fb.h"
#include "rtp/rtp_ext.h"
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
    pthread_mutex_lock(&publisher->media_lock);
    bool media_matches = publisher->uplink_video.ssrc == media_ssrc || publisher->uplink_video.rtx_ssrc == media_ssrc;
    pthread_mutex_unlock(&publisher->media_lock);
    if (!media_matches) {
      continue;
    }
    sfu_receiver_snapshot_t *snap = sfu_session_fanout_targets_acquire(publisher);
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
    sfu_subscriptions_snapshot_release(snap);
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

  uint32_t estimated_bps = sender_session->gcc_ctx ? sender_session->gcc_ctx->aimd.current_bitrate_bps : 0;
  uint32_t fresh_lost = 0;
  uint32_t fresh_total = 0;

  for (size_t i = 0; i < batch_count; i++) {
    if (!sender_session->twcc_history) {
      break;
    }
    if (batch[i].status == TWCC_STATUS_NOT_RECEIVED) {
      if (sfu_twcc_history_report_loss_once(sender_session->twcc_history, batch[i].sequence_number)) {
        fresh_lost++;
        fresh_total++;
      }
      continue;
    }

    gcc_packet_info_t item = {.sequence_number = batch[i].sequence_number, .receive_time_us = batch[i].receive_time_us};
    bool was_loss_reported = false;
    if (sfu_twcc_history_consume_received(sender_session->twcc_history, item.sequence_number, &item, &was_loss_reported)) {
      if (sender_session->gcc_ctx) {
        estimated_bps = gcc_bwe_process_twcc_packet(sender_session->gcc_ctx, &item);
      }
      if (!was_loss_reported) {
        fresh_total++;
      }
    }
  }

  if (parser.current_time_us > sender_session->twcc_last_feedback_ref_us) {
    sender_session->twcc_last_feedback_ref_us = parser.current_time_us;
  }

  if (sender_session->gcc_ctx && fresh_lost > 0) {
    gcc_bwe_report_loss(sender_session->gcc_ctx, fresh_lost, fresh_total);
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

    uint8_t orig_pkt[SFU_MAX_PAYLOAD_SIZE];
    uint32_t orig_len = 0;
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_pt = 0;

    if (!sfu_rtx_cache_get_stream(sender_session->rtx_cache, lost_seq, orig_pkt, &orig_len, &rtx_ssrc, &rtx_pt, nack_media_ssrc, nack_generation)) {
      unrecoverable_loss = true;
      continue;
    }

    if (!sfu_pacer_rtx_allow(&sender_session->pacer, orig_len + 2, (int64_t)sfu_now_us())) {
      sfu_metric_inc("rtx_dropped_budget");
      continue;
    }

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

    int64_t send_time_us = (int64_t)sfu_now_us();
    if (sender_session->twcc_send_extmap_id != 0) {
      uint16_t twcc_seq = __atomic_fetch_add(&sender_session->next_twcc_seq, 1, __ATOMIC_RELAXED);
      size_t rewritten_len = rtx_built_len;
      if (sfu_rtp_ext_write_twcc(rtx_enc->data, rtx_built_len, rtx_enc->cap, sender_session->twcc_send_extmap_id, twcc_seq, &rewritten_len)) {
        rtx_built_len = rewritten_len;
        if (sender_session->twcc_history) {
          sfu_twcc_history_record(sender_session->twcc_history, twcc_seq, send_time_us, (uint32_t)rtx_built_len);
        }
      } else {
        sfu_metric_inc("twcc_write_fail");
      }
    }

    int rtx_enc_len = (int)rtx_built_len;
    if (sfu_srtp_protect_rtp(&sender_session->srtp, rtx_enc->data, &rtx_enc_len, rtx_enc->cap)) {
      rtx_enc->len = (uint32_t)rtx_enc_len;
      sfu_ring_queue_send_zc(&w->send_ring, rtx_enc, (const struct sockaddr *)&sender_session->cold->addr, sender_session->cold->addr_len);
    }
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, rtx_enc);
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

typedef enum sfu_svc_parse_status {
  SFU_SVC_NOT_PRESENT = 0,
  SFU_SVC_PARSE_OK,
  SFU_SVC_PARSE_MALFORMED,
} sfu_svc_parse_status_t;

static sfu_svc_parse_status_t extract_svc_metadata(sfu_peer_session_t *sender_session, sfu_ingress_media_t *m) {
  m->has_svc = false;
  m->is_keyframe = false;

  pthread_mutex_lock(&sender_session->media_lock);
  uint8_t video_pt = sender_session->uplink_video.payload_type;
  sfu_video_codec_t codec = sender_session->uplink_video.codec;
  pthread_mutex_unlock(&sender_session->media_lock);

  if (m->is_audio || m->rtp.payload_type != video_pt || codec != SFU_VIDEO_CODEC_VP9) {
    return SFU_SVC_NOT_PRESENT;
  }

  if (sfu_svc_parse_descriptor(codec, m->rtp.payload, m->rtp.payload_len, &m->svc) != 0) {
    return SFU_SVC_PARSE_MALFORMED;
  }

  m->svc.rtp_timestamp = m->rtp.timestamp;
  m->has_svc = true;
  m->is_keyframe = sfu_svc_descriptor_is_keyframe(&m->svc);
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
  if (!is_rtcp && atomic_load_explicit(&sender_session->is_audience, memory_order_acquire)) {
    sfu_metric_inc("audience_rtp_drop");
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

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

  pthread_mutex_lock(&sender_session->media_lock);
  uint8_t twcc_recv_extmap_id = sender_session->twcc_recv_extmap_id;
  pthread_mutex_unlock(&sender_session->media_lock);
  if (sender_session->twcc_recv && twcc_recv_extmap_id != 0 && m.rtp.extension) {
    uint16_t twcc_seq = 0;
    if (sfu_rtp_ext_read_twcc(m.rtp.extension_profile, m.rtp.extension_data, m.rtp.extension_length, twcc_recv_extmap_id, &twcc_seq)) {
      int64_t arrival_us;
      if (pkt->recv_ts_ns != 0) {
        arrival_us = (int64_t)(pkt->recv_ts_ns / 1000ULL);
        sender_session->twcc_recv->have_kernel_clock = true;
        sender_session->twcc_recv->last_arrival_us = arrival_us;
      } else if (sender_session->twcc_recv->have_kernel_clock && sender_session->twcc_recv->last_arrival_us != 0) {
        arrival_us = sender_session->twcc_recv->last_arrival_us + 1;
        sender_session->twcc_recv->last_arrival_us = arrival_us;
      } else {
        arrival_us = (int64_t)sfu_now_us();
        sender_session->twcc_recv->last_arrival_us = arrival_us;
      }
      sfu_twcc_recv_tracker_record(sender_session->twcc_recv, twcc_seq, arrival_us);
    }
  }

  pthread_mutex_lock(&sender_session->media_lock);
  uint8_t in_pt = m.rtp.payload_type;
  bool is_video_pt = (in_pt == sender_session->uplink_video.payload_type) || (in_pt == sender_session->uplink_video.rtx_payload_type);
  bool send_negotiated = is_video_pt ? atomic_load_explicit(&sender_session->video_send_negotiated, memory_order_acquire)
                                     : atomic_load_explicit(&sender_session->audio_send_negotiated, memory_order_acquire);
  bool learned = false;
  if (!send_negotiated) {
    pthread_mutex_unlock(&sender_session->media_lock);
    sfu_metric_inc("unnegotiated_rtp_drop");
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }
  if (is_video_pt) {
    if (in_pt == sender_session->uplink_video.rtx_payload_type) {
      if (m.rtp.ssrc != 0 && sender_session->uplink_video.rtx_ssrc != m.rtp.ssrc) {
        sender_session->uplink_video.rtx_ssrc = m.rtp.ssrc;
        learned = true;
      }
    } else if (m.rtp.ssrc != 0 && sender_session->uplink_video.ssrc != m.rtp.ssrc) {
      sender_session->uplink_video.ssrc = m.rtp.ssrc;
      learned = true;
    }
    if (!sender_session->uplink_video.active) {
      sender_session->uplink_video.active = true;
      learned = true;
    }
  } else {
    if (m.rtp.ssrc != 0 && sender_session->uplink_audio.ssrc != m.rtp.ssrc) {
      sender_session->uplink_audio.ssrc = m.rtp.ssrc;
      learned = true;
    }
    if (!sender_session->uplink_audio.active) {
      sender_session->uplink_audio.active = true;
      learned = true;
    }
  }
  pthread_mutex_unlock(&sender_session->media_lock);
  if (learned) {
    atomic_store_explicit(&sender_session->uplink_ssrc_dirty, true, memory_order_release);
  }

  m.is_audio = !is_video_pt;
  sfu_svc_parse_status_t svc_status = extract_svc_metadata(sender_session, &m);
  if (svc_status == SFU_SVC_PARSE_MALFORMED) {
    sfu_metric_inc("vp9_descriptor_parse_fail");
    sfu_worker_request_keyframe_throttled(w, sender_session);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    sfu_session_release(sender_session);
    return;
  }

  sfu_router_forward(w, sender_session, &m);

  if (atomic_exchange_explicit(&sender_session->uplink_ssrc_dirty, false, memory_order_acq_rel) && sender_session->room) {
    pthread_mutex_lock(&sender_session->media_lock);
    uint32_t learned_audio_ssrc = sender_session->uplink_audio.ssrc;
    uint32_t learned_video_ssrc = sender_session->uplink_video.ssrc;
    pthread_mutex_unlock(&sender_session->media_lock);
    SFU_LOG_INFO("worker %u: learned uplink SSRCs from RTP for ufrag=%s (audio=%u video=%u); refreshing + renegotiating", w->worker_index,
                 sender_session->cold->ufrag, learned_audio_ssrc, learned_video_ssrc);
    room_refresh_peer_streams((sfu_room_t *)sender_session->room, sender_session);
    sfu_signaling_trigger_renegotiation((sfu_room_t *)sender_session->room);
  }
  sfu_session_release(sender_session);
}
