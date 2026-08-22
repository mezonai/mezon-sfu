#include "pipeline/egress.h"

#include "congestion/pacer.h"
#include "congestion/twcc_history.h"
#include "media/svc/layer_scheduler.h"
#include "net/io_uring.h"
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

static bool sfu_egress_process_local(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                                     const sfu_egress_media_t *media, sfu_pacer_class_t video_class, sfu_layer_scheduler_t *sched,
                                     const sfu_layer_scheduler_decision_t *decision, bool profile) {
  int enc_len = (int)pkt->len;

  if (!sfu_session_remote_slot_authorized(sub_session, media->remote_slot, media->assignment_generation)) {
    sfu_metric_inc("egress_mid_not_negotiated");
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
  if (!sfu_pacer_should_send(&sub_session->egress.pacer, cls, (uint32_t)enc_len + 10 /* SRTP auth tag */, &send_time_us)) {
    sfu_metric_inc("pacer_dropped_enh");
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
    return false;
  }
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
      if (!sched || !sfu_layer_scheduler_prepare_packet(sched, &media->svc, media->is_keyframe, &decision)) {
        if (sched) {
          sfu_layer_scheduler_reject_packet(sched, &decision);
        }
        if (reserved_output) {
          sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, reserved_output);
        }
        sfu_metric_inc("egress_admission_drop");
        return false;
      }
      video_class = decision.pacer_class;
      if (sched->needs_keyframe || sched->target_sid > sched->current_sid) {
        sfu_worker_request_keyframe_throttled_for_source(w, media->publisher, media->source);
      }
    }
  }

  if (video_class == SFU_PACER_CLASS_VIDEO_ENH) {
    int64_t now_us = (int64_t)sfu_now_us();
    if (sfu_pacer_debt_after(&sub_session->egress.pacer, plain->len + 10, now_us) > sub_session->egress.pacer.bucket_cap_bytes) {
      if (has_decision) {
        sfu_layer_scheduler_reject_packet(sched, &decision);
      }
      if (reserved_output) {
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, reserved_output);
      }
      sfu_metric_inc("egress_admission_drop");
      return false;
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
        sfu_worker_request_keyframe_throttled_for_source(w, media->publisher, media->source);
      }
      if (!sfu_layer_scheduler_prepare_packet(sched, &media->svc, media->is_keyframe, &decision)) {
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
