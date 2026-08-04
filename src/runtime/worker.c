#include "runtime/worker.h"
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include "congestion/gcc.h"
#include "congestion/twcc_history.h"
#include "congestion/twcc_parser.h"
#include "media/svc/vp9_parser.h"
#include "peer/session.h"
#include "pipeline/dispatch.h"
#include "rtcp/rtcp_compound.h"
#include "rtcp/rtcp_fb.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtp_packet.h"
#include "rtp/rtx.h"
#include "rtp/rtx_build.h"
#include "runtime/cpu.h"
#include "runtime/scheduler.h"
#include "runtime/signal.h"
#include "runtime/timer.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"
#include "util/metrics.h"
#include "util/netbytes.h"

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

  if (sfu_spsc_ring_init(&w->release_to_dispatcher, SFU_RELEASE_QUEUE_CAPACITY) != 0) {
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

static int sfu_rtp_get_payload_offset(const uint8_t *data, uint32_t len) {
  sfu_rtp_packet_t packet;
  if (!sfu_rtp_packet_parse(data, len, &packet)) {
    return -1;
  }
  return (int)packet.header_len;
}

#define SFU_WORKER_NACK_REQUEST_CAP 48

#define SFU_WORKER_KF_THROTTLE_MS 1000

static void sfu_svc_update_layers(sfu_peer_session_t *session, uint32_t bitrate_bps) {
  if (!session->scheduler) {
    return;
  }
  sfu_subscriber_scheduler_set_bitrate(session->scheduler, bitrate_bps);
}

void sfu_test_svc_update_layers(sfu_peer_session_t *session, uint32_t bitrate_bps) { sfu_svc_update_layers(session, bitrate_bps); }

static sfu_peer_session_t *sfu_worker_find_publisher_by_media_ssrc(sfu_peer_session_t *subscriber, uint32_t media_ssrc) {
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

static void sfu_worker_request_keyframe_throttled(sfu_worker_t *w, sfu_peer_session_t *publisher) {
  int64_t now = (int64_t)sfu_now_ms();
  if (now - publisher->last_pli_time > SFU_WORKER_KF_THROTTLE_MS) {
    publisher->last_pli_time = now;
    sfu_session_request_keyframe(w, publisher, false);  // false = PLI
  }
}

static void sfu_worker_request_source_keyframe(sfu_worker_t *w, sfu_peer_session_t *feedback_session, uint32_t media_ssrc) {
  sfu_peer_session_t *publisher = sfu_worker_find_publisher_by_media_ssrc(feedback_session, media_ssrc);
  if (!publisher) {
    sfu_metric_inc("rtcp_kf_unresolved");
    publisher = feedback_session;
    atomic_fetch_add_explicit(&publisher->refcount, 1, memory_order_relaxed);
  }
  sfu_worker_request_keyframe_throttled(w, publisher);
  sfu_session_release(publisher);
}

#define SFU_WORKER_TWCC_BATCH_CAP 256

static void sfu_worker_handle_twcc_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_twcc_parser_t parser;
  if (sfu_twcc_parser_init(&parser, view->member, view->member_len, sender_session->twcc_last_feedback_ref_us) != 0) {
    sfu_metric_inc("rtcp_twcc_bad");
    return;
  }

  gcc_packet_info_t batch[SFU_WORKER_TWCC_BATCH_CAP];
  size_t batch_count = 0;
  gcc_packet_info_t item;
  while (batch_count < SFU_WORKER_TWCC_BATCH_CAP && sfu_twcc_parser_next(&parser, &item)) {
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

static void sfu_worker_handle_nack_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
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

  uint16_t requested[SFU_WORKER_NACK_REQUEST_CAP];
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
    if (requested_count >= SFU_WORKER_NACK_REQUEST_CAP) {
      capped = true;
      break;
    }
    requested[requested_count++] = lost_seq;

    if (sender_session->scheduler && !sfu_pacer_rtx_allow(&sender_session->scheduler->pacer, 1200 /* conservative MTU estimate */, (int64_t)sfu_now_us())) {
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
    sfu_worker_request_source_keyframe(w, sender_session, nack_media_ssrc);
  }
}

static void sfu_worker_handle_pli_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_rtcp_pli pli;
  if (!sfu_rtcp_parse_pli(view, &pli)) {
    sfu_metric_inc("rtcp_pli_bad");
    return;
  }

  sfu_worker_request_source_keyframe(w, sender_session, pli.media_ssrc);
}

static void sfu_worker_handle_fir_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_rtcp_fir fir;
  if (!sfu_rtcp_parse_fir(view, &fir)) {
    sfu_metric_inc("rtcp_pli_bad");
    return;
  }
  SFU_LOG_DEBUG("worker %u: RTCP FIR from peer %u (media_ssrc=%u, %zu entries) ignored", w->worker_index, sender_session->peer_id, fir.media_ssrc,
                fir.entry_count);
}

static void sfu_worker_egress_process(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst,
                                      socklen_t dst_len, uint32_t video_ssrc, uint8_t video_pt, uint8_t video_rtx_pt, uint32_t video_rtx_ssrc, bool has_video,
                                      bool is_audio, sfu_pacer_class_t video_class) {
  int enc_len = (int)pkt->len;

  uint8_t incoming_pt = pkt->data[1] & 0x7F;
  uint8_t expected_pt = sfu_session_get_mapped_pt(sub_session, incoming_pt);
  if (incoming_pt != expected_pt) {
    pkt->data[1] = (pkt->data[1] & 0x80) | (expected_pt & 0x7F);
  }

  int64_t send_time_us = (int64_t)sfu_now_us();
  if (sub_session->scheduler) {
    sfu_pacer_class_t cls = is_audio ? SFU_PACER_CLASS_AUDIO : (has_video ? video_class : SFU_PACER_CLASS_VIDEO_BASE);
    if (!sfu_pacer_should_send(&sub_session->scheduler->pacer, cls, (uint32_t)enc_len + 10 /* SRTP auth tag */, &send_time_us)) {
      sfu_metric_inc("pacer_dropped_enh");
      return;
    }
  }

  uint16_t subscriber_seq = sfu_read_be16(pkt->data + 2);
  if (has_video && incoming_pt == video_pt && sub_session->rtx_cache) {
    sfu_rtx_cache_put_stream(sub_session->rtx_cache, subscriber_seq, pkt->data, (uint32_t)enc_len, video_rtx_ssrc, video_rtx_pt, video_ssrc,
                             atomic_load_explicit(&sub_session->egress_generation, memory_order_acquire));
  }

  if (sub_session->twcc_extmap_id != 0) {
    uint16_t twcc_seq = __atomic_fetch_add(&sub_session->next_twcc_seq, 1, __ATOMIC_RELAXED);
    size_t new_len = (size_t)enc_len;
    if (sfu_rtp_ext_write_twcc(pkt->data, (size_t)enc_len, pkt->cap, sub_session->twcc_extmap_id, twcc_seq, &new_len)) {
      enc_len = (int)new_len;
      if (sub_session->twcc_history) {
        sfu_twcc_history_record(sub_session->twcc_history, twcc_seq, send_time_us, (uint32_t)enc_len);
      }
    } else {
      sfu_metric_inc("twcc_write_fail");
    }
  }

  if (!sfu_srtp_protect_rtp(&sub_session->srtp, pkt->data, &enc_len, pkt->cap)) {
    SFU_LOG_WARN("worker %u: [EGRESS DROP] SRTP protect FAILED", w->worker_index);
    sfu_metric_inc("egress_protect_fail");
    return;
  }
  pkt->len = (uint32_t)enc_len;

  if (sfu_ring_queue_send_zc(&w->send_ring, pkt, (const struct sockaddr *)dst, dst_len) != 0) {
    SFU_LOG_WARN("worker %u: [EGRESS DROP] send SQ full", w->worker_index);
    sfu_metric_inc("egress_send_full");
  }
}

static void sfu_worker_handle_rtcp(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_packet_t *pkt) {
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
          sfu_worker_handle_twcc_member(w, sender_session, &view);
        } else if (view.fmt_count == 1) {
          sfu_worker_handle_nack_member(w, sender_session, &view);
        } else {
          sfu_metric_inc("rtcp_member_unknown");
        }
        break;
      case 206:  // PSFB
        if (view.fmt_count == 1) {
          sfu_worker_handle_pli_member(w, sender_session, &view);
        } else if (view.fmt_count == 4) {
          sfu_worker_handle_fir_member(w, sender_session, &view);
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

void sfu_room_forward_packet(sfu_worker_t *w, sfu_packet_t *pkt) {
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
    sfu_worker_handle_rtcp(w, sender_session, pkt); /* consumes pkt */
    sfu_session_release(sender_session);
    return;
  }

  bool is_vp9 = false;
  sfu_vp9_descriptor_t vp9_desc = {0};
  bool is_keyframe = false;
  uint8_t ingress_pt = pkt->data[1] & 0x7F;
  bool is_audio = sender_session->uplink_audio.active && ingress_pt == sender_session->uplink_audio.payload_type;

  if (!is_rtcp && !is_audio) {
    uint8_t incoming_pt = ingress_pt;
    if (incoming_pt == sender_session->uplink_video.payload_type) {
      is_vp9 = true;
      int payload_offset = sfu_rtp_get_payload_offset(pkt->data, pkt->len);

      if (payload_offset > 0) {
        const uint8_t *payload = pkt->data + payload_offset;
        size_t payload_len = pkt->len - payload_offset;
        if (sfu_parse_vp9_descriptor(payload, payload_len, &vp9_desc) == 0) {
          is_keyframe = (vp9_desc.p_bit == 0 && vp9_desc.sid == 0);
        } else {
          is_vp9 = false;
        }
      }
    }
  }

  sfu_receiver_snapshot_t *snap = sfu_session_receivers_acquire(sender_session);
  uint32_t receiver_count = snap ? snap->count : 0;

  for (uint32_t i = 0; i < receiver_count; i++) {
    const sfu_receiver_entry_t *slot = &snap->entries[i];

    sfu_peer_session_t *sub_session = slot->subscriber;
    if (!sub_session || sub_session->state != SFU_SESSION_ESTABLISHED) {
      continue;
    }

    if (!sub_session->scheduler) {
      continue;
    }

    if (sub_session->scheduler->active_publisher_id != sender_session->peer_id) {
      SFU_LOG_DEBUG("miss match active_publisher_id=%d, peer_id=%d", sub_session->scheduler->active_publisher_id, sender_session->peer_id);
      continue;
    }

    sfu_pacer_class_t video_class = SFU_PACER_CLASS_VIDEO_BASE;
    if (is_vp9 && slot->has_video) {
      bool should_forward = sfu_scheduler_evaluate_frame(sub_session->scheduler, &vp9_desc, is_keyframe);
      if (!should_forward) {
        continue;
      }
      video_class = sfu_scheduler_classify_frame(sub_session->scheduler, &vp9_desc);
    }

    sfu_packet_t *enc = sfu_packet_pool_alloc(w->pp);
    if (!enc) {
      SFU_LOG_WARN("worker %u: packet pool exhausted", w->worker_index);
      continue;
    }
    if (pkt->len > enc->cap) {
      SFU_LOG_WARN("worker %u: plaintext too large", w->worker_index);
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      continue;
    }

    memcpy(enc->data, pkt->data, pkt->len);
    enc->len = pkt->len;

    if (sub_session->worker_id == w->worker_index) {
      sfu_worker_egress_process(w, sub_session, enc, &sub_session->cold->addr, sub_session->cold->addr_len, slot->video_ssrc, slot->video_pt,
                                slot->video_rtx_pt, slot->video_rtx_ssrc, slot->has_video, is_audio, video_class);
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
    } else {
      atomic_fetch_add_explicit(&sub_session->refcount, 1, memory_order_relaxed);
      if (!sfu_fanout_mesh_enqueue_forward(w->mesh, w->worker_index, sub_session->worker_id, enc, sub_session, &sub_session->cold->addr,
                                           sub_session->cold->addr_len, slot->video_ssrc, slot->video_rtx_ssrc, slot->video_pt, slot->video_rtx_pt,
                                           slot->has_video, is_audio, (uint8_t)video_class)) {
        SFU_LOG_WARN("worker %u: fanout queue full", w->worker_index);
        sfu_session_release(sub_session);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      }
    }
  }

  sfu_receiver_snapshot_release(snap);
  sfu_session_release(sender_session);
  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
}

void sfu_worker_handle_fanout_job(void *user_data, sfu_fanout_job_t *job) {
  sfu_worker_t *w = (sfu_worker_t *)user_data;

  if (job->kind == SFU_FANOUT_JOB_FORWARD && job->subscriber) {
    sfu_worker_egress_process(w, job->subscriber, job->pkt, &job->dst, job->dst_len, job->video_ssrc, job->video_pt, job->video_rtx_pt, job->video_rtx_ssrc,
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
