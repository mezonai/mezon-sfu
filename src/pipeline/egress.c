#include "pipeline/egress.h"

#include "congestion/pacer.h"
#include "congestion/twcc_history.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/keyframe.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtp_packet.h"
#include "rtp/rtx.h"
#include "runtime/fanout.h"
#include "runtime/scheduler.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"
#include "util/metrics.h"
#include "util/netbytes.h"

static bool sfu_egress_process_local(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                                     const sfu_egress_media_t *media, sfu_pacer_class_t video_class, sfu_subscriber_scheduler_t *sched,
                                     const sfu_scheduler_decision_t *decision) {
  int enc_len = (int)pkt->len;

  uint8_t incoming_pt = pkt->data[1] & 0x7F;
  if (!media->is_audio && media->has_video && media->video_pt != 0 && incoming_pt != media->video_pt) {
    pkt->data[1] = (pkt->data[1] & 0x80) | (media->video_pt & 0x7F);
  }

  int64_t send_time_us = (int64_t)sfu_now_us();
  sfu_pacer_class_t cls = media->is_audio ? SFU_PACER_CLASS_AUDIO : (media->has_video ? video_class : SFU_PACER_CLASS_VIDEO_BASE);
  if (!sfu_pacer_should_send(&sub_session->pacer, cls, (uint32_t)enc_len + 10 /* SRTP auth tag */, &send_time_us)) {
    sfu_metric_inc("pacer_dropped_enh");
    return false;
  }

  uint16_t subscriber_seq = sfu_read_be16(pkt->data + 2);
  if (sched && decision) {
    subscriber_seq = sfu_scheduler_assign_output_seq(sched, subscriber_seq);
    if (!sfu_rtp_packet_set_seq(pkt->data, pkt->len, subscriber_seq) || !sfu_rtp_packet_set_marker(pkt->data, pkt->len, decision->set_marker)) {
      return false;
    }
  }
  if (media->has_video && !media->is_audio && sub_session->rtx_cache) {
    sfu_rtx_cache_put_stream(sub_session->rtx_cache, subscriber_seq, pkt->data, (uint32_t)enc_len, media->video_rtx_ssrc, media->video_rtx_pt,
                             media->video_ssrc, atomic_load_explicit(&sub_session->egress_generation, memory_order_acquire));
  }

  pthread_mutex_lock(&sub_session->media_lock);
  uint8_t twcc_send_extmap_id = sub_session->twcc_send_extmap_id;
  pthread_mutex_unlock(&sub_session->media_lock);
  if (twcc_send_extmap_id != 0) {
    uint16_t twcc_seq = __atomic_fetch_add(&sub_session->next_twcc_seq, 1, __ATOMIC_RELAXED);
    size_t new_len = (size_t)enc_len;
    if (sfu_rtp_ext_write_twcc(pkt->data, (size_t)enc_len, pkt->cap, twcc_send_extmap_id, twcc_seq, &new_len)) {
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
    return false;
  }
  pkt->len = (uint32_t)enc_len;

  if (sfu_ring_queue_send_zc(&w->send_ring, pkt, (const struct sockaddr *)dst, dst_len) != 0) {
    SFU_LOG_WARN("worker %u: [EGRESS DROP] send SQ full", w->worker_index);
    sfu_metric_inc("egress_send_full");
    return false;
  }
  return true;
}

bool sfu_egress_process(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                        const sfu_egress_media_t *media) {
  if (!w || !sub_session || !pkt || !dst || !media || sub_session->worker_id != w->worker_index) {
    if (w && pkt) {
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    }
    return false;
  }

  sfu_pacer_class_t video_class = SFU_PACER_CLASS_VIDEO_BASE;
  sfu_subscriber_scheduler_t *sched = NULL;
  sfu_scheduler_decision_t decision;
  bool has_decision = media->has_svc && media->has_video && !media->is_audio;

  if (has_decision) {
    if (!media->publisher || media->publisher->peer_id == 0) {
      SFU_LOG_WARN("worker %u: [EGRESS] VP9 without publisher peer_id; forwarding without layer filter", w->worker_index);
      has_decision = false;
    } else {
      sched = sfu_session_scheduler_for(sub_session, media->publisher->peer_id);
      if (!sched) {
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
        return false;
      }
      if (sched->needs_keyframe || sched->target_sid > sched->current_sid) {
        sfu_worker_request_keyframe_throttled(w, media->publisher);
      }
      if (!sfu_scheduler_prepare_packet(sched, &media->svc, media->is_keyframe, &decision)) {
        sfu_scheduler_reject_packet(sched, &decision);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
        return false;
      }
      video_class = decision.pacer_class;
    }
  }

  bool admitted = sfu_egress_process_local(w, sub_session, pkt, dst, dst_len, media, video_class, sched, has_decision ? &decision : NULL);
  if (has_decision) {
    if (admitted) {
      sfu_scheduler_commit_packet(sched, &decision);
    } else {
      sfu_scheduler_reject_packet(sched, &decision);
    }
  }
  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
  return admitted;
}
