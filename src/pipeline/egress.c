#include "pipeline/egress.h"

#include "congestion/pacer.h"
#include "congestion/twcc_history.h"
#include "media/svc/layer_scheduler.h"
#include "net/io_backend.h"
#include "peer/session.h"
#include "pipeline/keyframe.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtp_packet.h"
#include "rtp/rtx.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"
#include "util/metrics.h"
#include "util/netbytes.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

static inline uint64_t sfu_egress_profile_cycles(void) {
#if defined(__x86_64__) || defined(__i386__)
  unsigned aux;
  return __rdtscp(&aux);
#else
  return 0;
#endif
}

#ifdef SFU_DIAG_LOG
static const char *sfu_vp9_reject_reason_name(sfu_layer_reject_reason_t reason) {
  switch (reason) {
    case SFU_LAYER_REJECT_INVALID_SID: return "invalid_sid";
    case SFU_LAYER_REJECT_OVER_TARGET_OR_FAILED: return "over_target_or_failed";
    case SFU_LAYER_REJECT_MISSING_FRAME_START: return "missing_frame_start";
    case SFU_LAYER_REJECT_KEYFRAME_REQUIRED: return "keyframe_required";
    case SFU_LAYER_REJECT_KEYFRAME_MISMATCH: return "keyframe_mismatch";
    case SFU_LAYER_REJECT_SPATIAL_TARGET: return "spatial_target";
    case SFU_LAYER_REJECT_SPATIAL_TRANSITION: return "spatial_transition";
    case SFU_LAYER_REJECT_SPATIAL_START: return "spatial_start";
    case SFU_LAYER_REJECT_SPATIAL_DEPENDENCY: return "spatial_dependency";
    case SFU_LAYER_REJECT_TEMPORAL_TRANSITION: return "temporal_transition";
    case SFU_LAYER_REJECT_TEMPORAL_SWITCH: return "temporal_switch";
    case SFU_LAYER_REJECT_PACER_OVERLAP: return "pacer_overlap";
    case SFU_LAYER_REJECT_PACER_ORPHAN: return "pacer_orphan";
    default: return "none";
  }
}

static void sfu_log_vp9_scheduler_drop(const sfu_peer_session_t *sub_session, const sfu_egress_media_t *media, const sfu_layer_scheduler_t *sched,
                                       const sfu_layer_scheduler_decision_t *decision) {
  static _Atomic uint32_t drop_logs;
  uint32_t n = atomic_fetch_add_explicit(&drop_logs, 1, memory_order_relaxed);
  if (n != 0 && (n & 127u) != 0) return;
  SFU_LOG_WARN("[VP9-DBG] drop n=%u reason=%s sub=%u pub=%u source=%u ts=%u sid=%u tid=%u b=%u e=%u p=%u u=%u d=%u kf=%u "
               "target=%u/%u current=%u/%u need_kf=%u kf_active=%u kf_failed=%u spatial=%u/%u temporal=%u/%u pacer=%u pacer_ts=%u "
               "masks=%02x/%02x/%02x",
               n + 1, sfu_vp9_reject_reason_name(decision->reject_reason), sub_session->peer_id,
               media->publisher ? media->publisher->peer_id : 0, (unsigned)media->source, decision->rtp_timestamp, decision->sid, decision->tid,
               decision->b_bit, decision->e_bit, media->svc.p_bit, media->svc.u_bit, media->svc.d_bit, media->is_keyframe, sched->target_sid,
               sched->target_tid, sched->current_sid, sched->current_tid, sched->needs_keyframe, sched->keyframe_active, sched->keyframe_failed,
               sched->transition_active, sched->transition_failed, sched->temporal_transition_active, sched->temporal_transition_failed,
               sched->pacer_frame_active, sched->pacer_frame_timestamp, sched->started_sid_mask, sched->completed_sid_mask, sched->failed_sid_mask);
}

static void sfu_log_vp9_keyframe_intent(const sfu_peer_session_t *sub_session, const sfu_egress_media_t *media, const sfu_layer_scheduler_t *sched) {
  static _Atomic uint32_t keyframe_logs;
  uint32_t n = atomic_fetch_add_explicit(&keyframe_logs, 1, memory_order_relaxed);
  if (n != 0 && (n & 127u) != 0) return;
  SFU_LOG_WARN("[VP9-DBG] keyframe-intent n=%u sub=%u pub=%u source=%u ts=%u target=%u/%u current=%u/%u need_kf=%u spatial=%u/%u",
               n + 1, sub_session->peer_id, media->publisher ? media->publisher->peer_id : 0, (unsigned)media->source, media->svc.rtp_timestamp,
               sched->target_sid, sched->target_tid, sched->current_sid, sched->current_tid, sched->needs_keyframe, sched->transition_active,
               sched->transition_failed);
}
#endif

static bool sfu_egress_process_local(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                                     const sfu_egress_media_t *media, sfu_pacer_class_t video_class, sfu_layer_scheduler_t *sched,
                                     const sfu_layer_scheduler_decision_t *decision, bool profile) {
  int enc_len = (int)pkt->len;

  if (!sfu_session_remote_slot_authorized(sub_session, media->remote_slot, media->assignment_generation)) {
    sfu_metric_inc("egress_mid_not_negotiated");
#ifdef SFU_DIAG_LOG
    static _Atomic uint32_t unauthorized_logs;
    uint32_t n = atomic_fetch_add_explicit(&unauthorized_logs, 1, memory_order_relaxed);
    if (n == 0 || (n & 127u) == 0) {
      uint64_t applied = 0;
      if (media->remote_slot < SFU_MAX_REMOTE_SLOTS) {
        applied = atomic_load_explicit(&sub_session->graph.remote_slots.applied_assignment_generations[media->remote_slot], memory_order_acquire);
      }
      uint32_t mid = sfu_remote_slot_first_mid(media->remote_slot) + (media->is_audio ? 0u : media->source == SFU_MEDIA_SCREEN ? 2u : 1u);
      SFU_LOG_WARN("egress: unauthorized n=%u sub_peer=%u pub_peer=%u slot=%u mid=%u audio=%d source=%d route_gen=%" PRIu64 " applied_gen=%" PRIu64,
                   n + 1, sub_session->peer_id, media->publisher ? media->publisher->peer_id : 0, media->remote_slot, mid, media->is_audio ? 1 : 0,
                   (int)media->source, media->assignment_generation, applied);
    }
#endif
    return false;
  }

  uint32_t mid = sfu_remote_slot_first_mid(media->remote_slot) +
                 (media->is_audio ? 0u : media->source == SFU_MEDIA_SCREEN ? 2u : 1u);

  uint8_t incoming_pt = pkt->data[1] & 0x7F;
  if (!media->is_audio && media->has_video && media->video_pt != 0 && incoming_pt != media->video_pt) {
    pkt->data[1] = (pkt->data[1] & 0x80) | (media->video_pt & 0x7F);
  }

  int64_t send_time_us = (int64_t)sfu_now_us();
  sfu_pacer_class_t cls = media->is_audio ? SFU_PACER_CLASS_AUDIO : (media->has_video ? video_class : SFU_PACER_CLASS_VIDEO_BASE);
  bool allow_congestion_drop = decision && decision->pacer_frame_start;
  if (!sfu_pacer_should_send(&sub_session->egress.pacer, cls, (uint32_t)enc_len + 10 /* SRTP auth tag */, allow_congestion_drop, &send_time_us)) {
    sfu_metric_inc("pacer_dropped_enh");
    sfu_metric_inc("pacer_dropped_enh_frames");
    return false;
  }

  if (pkt->len < 12) {
    sfu_metric_inc("egress_seq_translate_fail");
    return false;
  }
  uint16_t source_seq = sfu_read_be16(pkt->data + 2);
  uint32_t outbound_ssrc = sfu_read_be32(pkt->data + 8);
  uint16_t subscriber_seq = 0;
  pthread_mutex_lock(&sub_session->crypto_lock);
  bool translated = sfu_rtp_seq_translate(&sub_session->cold->rtp_seq_translator, outbound_ssrc, source_seq, &subscriber_seq);
  pthread_mutex_unlock(&sub_session->crypto_lock);
  if (!translated) {
    sfu_metric_inc("egress_seq_translate_fail");
    sfu_metric_inc("egress_seq_translate_table_full");
    return false;
  }
  if (!sfu_rtp_packet_set_seq(pkt->data, pkt->len, subscriber_seq)) {
    sfu_metric_inc("egress_seq_translate_fail");
    return false;
  }
  if (sched && decision && !sfu_rtp_packet_set_marker(pkt->data, pkt->len, decision->set_marker)) {
    return false;
  }
  sfu_media_snapshot_t egress_msnap = sfu_session_load_media(sub_session);
  uint8_t mid_send_extmap_id = egress_msnap.mid_recv_extmap_id;
  if (mid_send_extmap_id != 0) {
    char mid_text[12];
    int mid_len = snprintf(mid_text, sizeof(mid_text), "%u", mid);
    size_t new_len = (size_t)enc_len;
    if (mid_len <= 0 || (size_t)mid_len >= sizeof(mid_text) ||
        !sfu_rtp_ext_write_mid(pkt->data, (size_t)enc_len, pkt->cap, mid_send_extmap_id, mid_text, &new_len)) {
      sfu_metric_inc("mid_write_fail");
#ifdef SFU_DIAG_LOG
      static _Atomic uint32_t mid_write_fail_logs;
      uint32_t n = atomic_fetch_add_explicit(&mid_write_fail_logs, 1, memory_order_relaxed);
      if (n == 0 || (n & 127u) == 0) {
        SFU_LOG_WARN("egress: mid_write_fail n=%u sub_peer=%u slot=%u mid=%u extmap=%u pkt_len=%d cap=%u", n + 1, sub_session->peer_id, media->remote_slot, mid,
                     mid_send_extmap_id, enc_len, pkt->cap);
      }
#endif
      return false;
    }
    enc_len = (int)new_len;
  }

  if (media->has_video && !media->is_audio && sfu_session_video_runtime_ready(sub_session) && sub_session->egress.rtx_cache) {
    sfu_rtx_cache_put_stream(sub_session->egress.rtx_cache, subscriber_seq, pkt->data, (uint32_t)enc_len, media->video_rtx_ssrc, media->video_rtx_pt,
                             media->video_ssrc, atomic_load_explicit(&sub_session->egress.generation, memory_order_acquire));
  }

  uint8_t twcc_send_extmap_id = egress_msnap.twcc_send_extmap_id;
  if (twcc_send_extmap_id != 0) {
    uint16_t twcc_seq = atomic_fetch_add_explicit(&sub_session->egress.next_twcc_seq, 1, memory_order_relaxed);
    size_t new_len = (size_t)enc_len;
    if (sfu_rtp_ext_write_twcc(pkt->data, (size_t)enc_len, pkt->cap, twcc_send_extmap_id, twcc_seq, &new_len)) {
      enc_len = (int)new_len;
      if (sfu_session_video_runtime_ready(sub_session) && sub_session->egress.twcc_history) {
        sfu_twcc_history_record(sub_session->egress.twcc_history, twcc_seq, send_time_us, (uint32_t)enc_len);
      }
    } else {
      sfu_metric_inc("twcc_write_fail");
    }
  }

  uint64_t crypto_start = profile ? sfu_egress_profile_cycles() : 0;
  pthread_mutex_lock(&sub_session->crypto_lock);
  srtp_err_status_t protect_status = sfu_srtp_protect_rtp_status(&sub_session->srtp, pkt->data, &enc_len, pkt->cap);
  pthread_mutex_unlock(&sub_session->crypto_lock);
  if (profile) {
    w->hot.crypto_cycles += sfu_egress_profile_cycles() - crypto_start;
  }
  if (protect_status != srtp_err_status_ok) {
    if (protect_status == srtp_err_status_replay_old) {
      sfu_metric_inc("egress_protect_replay_old");
    } else if (protect_status == srtp_err_status_replay_fail) {
      sfu_metric_inc("egress_protect_replay_fail");
    }
    sfu_metric_inc(media->is_audio ? "egress_protect_fail_audio" : "egress_protect_fail_video");
    sfu_metric_inc("egress_protect_fail");
#ifdef SFU_DIAG_LOG
    static _Atomic uint32_t protect_fail_logs;
    uint32_t n = atomic_fetch_add_explicit(&protect_fail_logs, 1, memory_order_relaxed);
    if (n == 0 || (n & 127u) == 0) {
      SFU_LOG_WARN("egress: protect_fail n=%u sub_peer=%u pub_peer=%u slot=%u audio=%d status=%d", n + 1, sub_session->peer_id,
                   media->publisher ? media->publisher->peer_id : 0, media->remote_slot, media->is_audio ? 1 : 0, (int)protect_status);
    }
#endif
    return false;
  }
#ifdef SFU_DIAG_LOG
  {
    static _Atomic uint32_t egress_ok_logs;
    uint32_t n = atomic_fetch_add_explicit(&egress_ok_logs, 1, memory_order_relaxed);
    if (n == 0 || (n & 127u) == 0) {
      SFU_LOG_INFO("egress: ok n=%u sub_peer=%u pub_peer=%u slot=%u mid=%u audio=%d ssrc=%" PRIu32 " pt=%u len=%d", n + 1, sub_session->peer_id,
                   media->publisher ? media->publisher->peer_id : 0, media->remote_slot, mid, media->is_audio ? 1 : 0, outbound_ssrc, incoming_pt, enc_len);
    }
  }
#endif
  pkt->len = (uint32_t)enc_len;

  if (sfu_ring_queue_send_zc(&w->send_ring, pkt, (const struct sockaddr *)dst, dst_len) != 0) {
    SFU_LOG_WARN("worker %u: [EGRESS DROP] send SQ full", w->worker_index);
    sfu_metric_inc("egress_send_full");
    sfu_metric_inc("send_sq_full");
    return false;
  }
  w->hot.output_queued++;
  return true;
}

static bool sfu_egress_process_plaintext_output(sfu_worker_t *w, sfu_peer_session_t *sub_session, const sfu_packet_t *plain, sfu_packet_t *reserved_output,
                                                const struct sockaddr_storage *dst, socklen_t dst_len, const sfu_egress_media_t *media) {
  if (!w || !sub_session || !plain || !dst || !media || sfu_session_owner_worker(sub_session) != w->worker_index || !sfu_session_accepts_work(sub_session)) {
    if (w && reserved_output) {
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, reserved_output);
    }
    return false;
  }

  sfu_pacer_class_t video_class = SFU_PACER_CLASS_VIDEO_BASE;
  sfu_layer_scheduler_t *sched = NULL;
  sfu_layer_scheduler_decision_t decision;
  bool has_decision = media->has_svc && media->has_video && !media->is_audio;
  if (has_decision) {
    if (!media->publisher || media->publisher->peer_id == 0) {
      has_decision = false;
    } else {
      sched = sfu_layer_scheduler_for_stream(sub_session, media->publisher->peer_id, media->source);
      if (!sched) {
        if (reserved_output) {
          sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, reserved_output);
        }
        sfu_metric_inc("egress_admission_drop");
        return false;
      }
      if (sched->needs_keyframe || sched->target_sid > sched->current_sid) {
#ifdef SFU_DIAG_LOG
        sfu_log_vp9_keyframe_intent(sub_session, media, sched);
#endif
        sfu_worker_request_keyframe_throttled_for_source(w, media->publisher, media->source);
      }
      if (!sfu_layer_scheduler_prepare_packet(sched, &media->svc, media->is_keyframe, &decision)) {
#ifdef SFU_DIAG_LOG
        sfu_log_vp9_scheduler_drop(sub_session, media, sched, &decision);
#endif
        if (decision.pacer_frame_continuation) {
          sfu_metric_inc("vp9_enh_orphan_continuation");
        }
        sfu_layer_scheduler_reject_packet(sched, &decision);
        if (reserved_output) {
          sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, reserved_output);
        }
        sfu_metric_inc("egress_admission_drop");
        return false;
      }
      video_class = decision.pacer_class;
    }
  }

  sfu_packet_t *output = reserved_output;
  if (!output) {
    output = sfu_worker_packet_arena_alloc(&w->output_arena);
    if (output) {
      w->hot.output_arena_allocated++;
    } else {
      output = sfu_packet_pool_alloc(w->pp);
      if (output) {
        w->hot.output_pool_fallback++;
      }
    }
  }
  if (output && plain->len > output->cap && output->buf_source == SFU_BUF_SOURCE_WORKER_ARENA) {
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, output);
    output = sfu_packet_pool_alloc(w->pp);
    if (output) {
      w->hot.output_pool_fallback++;
    }
  }
  if (!output || plain->len > output->cap) {
    if (output) {
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, output);
    }
    if (has_decision) {
      sfu_layer_scheduler_reject_packet(sched, &decision);
    }
    return false;
  }

  bool profile = (++w->hot.profile_sequence & 1023u) == 0;
  uint64_t copy_start = profile ? sfu_egress_profile_cycles() : 0;
  memcpy(output->data, plain->data, plain->len);
  if (profile) {
    w->hot.copy_cycles += sfu_egress_profile_cycles() - copy_start;
    w->hot.profile_samples++;
  }
  output->len = plain->len;
  w->hot.copied_bytes += output->len;
  sfu_metric_inc("egress_output_alloc");
  sfu_metric_add("egress_copied_bytes", output->len);

  bool admitted = sfu_egress_process_local(w, sub_session, output, dst, dst_len, media, video_class, sched, has_decision ? &decision : NULL, profile);
  if (has_decision) {
    if (admitted) {
      sfu_layer_scheduler_commit_packet(sched, &decision);
    } else {
      sfu_layer_scheduler_reject_packet(sched, &decision);
    }
  }
  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, output);
  return admitted;
}

bool sfu_egress_process_plaintext(sfu_worker_t *w, sfu_peer_session_t *sub_session, const sfu_packet_t *plain, const struct sockaddr_storage *dst,
                                  socklen_t dst_len, const sfu_egress_media_t *media) {
  return sfu_egress_process_plaintext_output(w, sub_session, plain, NULL, dst, dst_len, media);
}

bool sfu_egress_process_plaintext_reserved(sfu_worker_t *w, sfu_peer_session_t *sub_session, const sfu_packet_t *plain, sfu_packet_t *output,
                                           const struct sockaddr_storage *dst, socklen_t dst_len, const sfu_egress_media_t *media) {
  return sfu_egress_process_plaintext_output(w, sub_session, plain, output, dst, dst_len, media);
}

bool sfu_egress_process(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                        const sfu_egress_media_t *media) {
  if (!w || !sub_session || !pkt || !dst || !media || sfu_session_owner_worker(sub_session) != w->worker_index) {
    if (w && pkt) {
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    }
    return false;
  }

  sfu_pacer_class_t video_class = SFU_PACER_CLASS_VIDEO_BASE;
  sfu_layer_scheduler_t *sched = NULL;
  sfu_layer_scheduler_decision_t decision;
  bool has_decision = media->has_svc && media->has_video && !media->is_audio;

  if (has_decision) {
    if (!media->publisher || media->publisher->peer_id == 0) {
      SFU_LOG_WARN("worker %u: [EGRESS] VP9 without publisher peer_id; forwarding without layer filter", w->worker_index);
      has_decision = false;
    } else {
      sched = sfu_layer_scheduler_for_stream(sub_session, media->publisher->peer_id, media->source);
      if (!sched) {
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
        return false;
      }
      if (sched->needs_keyframe || sched->target_sid > sched->current_sid) {
#ifdef SFU_DIAG_LOG
        sfu_log_vp9_keyframe_intent(sub_session, media, sched);
#endif
        sfu_worker_request_keyframe_throttled_for_source(w, media->publisher, media->source);
      }
      if (!sfu_layer_scheduler_prepare_packet(sched, &media->svc, media->is_keyframe, &decision)) {
#ifdef SFU_DIAG_LOG
        sfu_log_vp9_scheduler_drop(sub_session, media, sched, &decision);
#endif
        if (decision.pacer_frame_continuation) {
          sfu_metric_inc("vp9_enh_orphan_continuation");
        }
        sfu_layer_scheduler_reject_packet(sched, &decision);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
        return false;
      }
      video_class = decision.pacer_class;
    }
  }

  bool admitted = sfu_egress_process_local(w, sub_session, pkt, dst, dst_len, media, video_class, sched, has_decision ? &decision : NULL, false);
  if (has_decision) {
    if (admitted) {
      sfu_layer_scheduler_commit_packet(sched, &decision);
    } else {
      sfu_layer_scheduler_reject_packet(sched, &decision);
    }
  }
  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
  return admitted;
}
