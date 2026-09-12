#include "congestion/probe_controller.h"

#include <stdlib.h>
#include <string.h>

#include "peer/session.h"
#include "pipeline/paced_send.h"
#include "room/room_media_graph.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtp_padding.h"
#include "rtp/rtp_seq_translate.h"
#include "rtp/rtx.h"
#include "runtime/timer.h"
#include "runtime/worker.h"
#include "transport/srtp/srtp.h"
#include "util/alloc.h"
#include "util/metrics.h"

sfu_probe_controller_t *sfu_probe_controller_create(void) {
  sfu_probe_controller_t *pc = SFU_CALLOC(1, sizeof(sfu_probe_controller_t));
  if (!pc) {
    return NULL;
  }
  sfu_probe_controller_init(pc);
  return pc;
}

void sfu_probe_controller_init(sfu_probe_controller_t *pc) {
  if (!pc) {
    return;
  }
  memset(pc, 0, sizeof(*pc));
  pthread_mutex_init(&pc->lock, NULL);
  pc->state = SFU_PROBE_STATE_IDLE;
}

void sfu_probe_controller_destroy(sfu_probe_controller_t *pc) {
  if (!pc) {
    return;
  }
  pthread_mutex_destroy(&pc->lock);
  SFU_FREE(pc);
}

void sfu_probe_controller_reset(sfu_probe_controller_t *pc) {
  if (!pc) {
    return;
  }
  pthread_mutex_lock(&pc->lock);
  pc->state = SFU_PROBE_STATE_IDLE;
  pc->bytes_sent = 0;
  pc->packets_sent = 0;
  pc->packets_received = 0;
  pc->bytes_received = 0;
  pc->packets_lost = 0;
  pc->start_time_us = 0;
  pc->send_end_time_us = 0;
  pc->first_recv_us = 0;
  pc->last_recv_us = 0;
  pc->aborted = false;
  pc->cooldown_until_us = 0;
  pthread_mutex_unlock(&pc->lock);
}

void sfu_probe_controller_set_unmet_demand(sfu_probe_controller_t *pc, bool has_unmet_demand) {
  if (!pc) {
    return;
  }
  pthread_mutex_lock(&pc->lock);
  pc->has_unmet_demand = has_unmet_demand;
  pthread_mutex_unlock(&pc->lock);
}

bool sfu_probe_controller_is_probing(sfu_probe_controller_t *pc) {
  if (!pc) {
    return false;
  }
  pthread_mutex_lock(&pc->lock);
  bool active = pc->state != SFU_PROBE_STATE_IDLE;
  pthread_mutex_unlock(&pc->lock);
  return active;
}

bool sfu_probe_controller_should_probe(sfu_probe_controller_t *pc, const gcc_bwe_context_t *gcc, int64_t now_us,
                                       bool twcc_negotiated, bool rtx_available,
                                       uint32_t rtx_queue_count, uint32_t media_backlog_count) {
  if (!pc || !gcc || !twcc_negotiated || !rtx_available) {
    return false;
  }
  if (rtx_queue_count > 0 || media_backlog_count > 0) {
    return false;
  }
  pthread_mutex_lock(&pc->lock);
  if (pc->state != SFU_PROBE_STATE_IDLE || !pc->has_unmet_demand || now_us < pc->cooldown_until_us) {
    pthread_mutex_unlock(&pc->lock);
    return false;
  }
  pthread_mutex_unlock(&pc->lock);

  if (gcc_bwe_is_overusing(gcc) || gcc_bwe_get_usage(gcc) != GCC_BWE_NORMAL) {
    return false;
  }
  return gcc_bwe_has_recent_feedback(gcc, now_us, 500000LL);
}

bool sfu_probe_controller_start_probe(sfu_probe_controller_t *pc, gcc_bwe_context_t *gcc, int64_t now_us) {
  if (!pc) {
    return false;
  }
  pthread_mutex_lock(&pc->lock);
  if (pc->state != SFU_PROBE_STATE_IDLE) {
    pthread_mutex_unlock(&pc->lock);
    return false;
  }

  uint32_t base_bps = gcc ? gcc_bwe_get_bitrate(gcc) : 0;
  if (base_bps == 0) {
    base_bps = 300000u;
  }
  if (base_bps >= SFU_PROBE_MAX_BITRATE_BPS) {
    /* Already at cap: probing cannot prove anything new. */
    pthread_mutex_unlock(&pc->lock);
    return false;
  }
  pc->cluster_id++;
  if (pc->cluster_id == 0) {
    pc->cluster_id = 1;
  }
  pc->state = SFU_PROBE_STATE_PROBING;
  pc->start_time_us = now_us;
  pc->send_end_time_us = 0;
  pc->first_recv_us = 0;
  pc->last_recv_us = 0;
  pc->bytes_sent = 0;
  pc->packets_sent = 0;
  pc->packets_received = 0;
  pc->bytes_received = 0;
  pc->packets_lost = 0;
  pc->aborted = false;

  pc->base_bitrate_bps = base_bps;
  uint32_t step = base_bps / 2;
  if (step < SFU_PROBE_MIN_STEP_BPS) {
    step = SFU_PROBE_MIN_STEP_BPS;
  }
  uint64_t target = (uint64_t)base_bps + step;
  if (target > SFU_PROBE_MAX_BITRATE_BPS) {
    target = SFU_PROBE_MAX_BITRATE_BPS;
  }
  pc->target_bitrate_bps = (uint32_t)target;
  pc->bytes_target = (uint32_t)((uint64_t)pc->target_bitrate_bps * SFU_PROBE_CLUSTER_DURATION_US / 8000000ULL);
  if (pc->bytes_target < 750) {
    pc->bytes_target = 750;
  }

  pc->clusters_started++;
  if (gcc) {
    gcc_bwe_set_active_probing(gcc, true);
  }
  sfu_metric_inc("congestion_probe_started");
  pthread_mutex_unlock(&pc->lock);
  return true;
}

void sfu_probe_controller_on_packet_received(sfu_probe_controller_t *pc, uint32_t cluster_id, uint32_t size_bytes,
                                             int64_t send_time_us, int64_t recv_time_us) {
  (void)send_time_us;
  if (!pc) {
    return;
  }
  pthread_mutex_lock(&pc->lock);
  if (pc->state != SFU_PROBE_STATE_IDLE && pc->cluster_id == cluster_id) {
    pc->packets_received++;
    pc->bytes_received += size_bytes;
    if (pc->first_recv_us == 0 || recv_time_us < pc->first_recv_us) {
      pc->first_recv_us = recv_time_us;
    }
    if (recv_time_us > pc->last_recv_us) {
      pc->last_recv_us = recv_time_us;
    }
  }
  pthread_mutex_unlock(&pc->lock);
}

void sfu_probe_controller_on_packet_lost(sfu_probe_controller_t *pc, uint32_t cluster_id) {
  if (!pc) {
    return;
  }
  pthread_mutex_lock(&pc->lock);
  if (pc->state != SFU_PROBE_STATE_IDLE && pc->cluster_id == cluster_id) {
    pc->packets_lost++;
  }
  pthread_mutex_unlock(&pc->lock);
}

void sfu_probe_controller_abort(sfu_probe_controller_t *pc, gcc_bwe_context_t *gcc, int64_t now_us, const char *reason) {
  (void)reason;
  if (!pc) {
    return;
  }
  pthread_mutex_lock(&pc->lock);
  if (pc->state != SFU_PROBE_STATE_IDLE) {
    pc->state = SFU_PROBE_STATE_IDLE;
    pc->aborted = true;
    pc->cooldown_until_us = now_us + SFU_PROBE_COOLDOWN_FAILURE_US;
    pc->clusters_aborted++;
    if (gcc) {
      gcc_bwe_set_active_probing(gcc, false);
    }
    sfu_metric_inc("congestion_probe_aborted");
  }
  pthread_mutex_unlock(&pc->lock);
}

bool sfu_probe_controller_check_cluster_done(sfu_probe_controller_t *pc, gcc_bwe_context_t *gcc, int64_t now_us,
                                             uint32_t *out_probe_bitrate_bps) {
  if (out_probe_bitrate_bps) {
    *out_probe_bitrate_bps = 0;
  }
  if (!pc) {
    return false;
  }
  pthread_mutex_lock(&pc->lock);
  if (pc->state == SFU_PROBE_STATE_IDLE) {
    pthread_mutex_unlock(&pc->lock);
    return false;
  }

  if (pc->aborted) {
    pc->state = SFU_PROBE_STATE_IDLE;
    pc->aborted = false;
    if (gcc) {
      gcc_bwe_set_active_probing(gcc, false);
    }
    pthread_mutex_unlock(&pc->lock);
    return false;
  }

  if (gcc && gcc_bwe_is_overusing(gcc)) {
    pc->state = SFU_PROBE_STATE_IDLE;
    pc->cooldown_until_us = now_us + SFU_PROBE_COOLDOWN_FAILURE_US;
    pc->clusters_failed++;
    gcc_bwe_set_active_probing(gcc, false);
    sfu_metric_inc("congestion_probe_failed");
    pthread_mutex_unlock(&pc->lock);
    return true;
  }

  bool all_feedback = pc->packets_sent > 0 && (pc->packets_received + pc->packets_lost >= pc->packets_sent);
  bool timed_out = pc->send_end_time_us > 0 && (now_us - pc->send_end_time_us > SFU_PROBE_CLUSTER_TIMEOUT_US);

  if (!all_feedback && !timed_out) {
    pthread_mutex_unlock(&pc->lock);
    return false;
  }

  uint64_t measured_bps = 0;
  if (pc->packets_received >= 2) {
    int64_t span_us = pc->last_recv_us - pc->first_recv_us;
    if (span_us > 0) {
      measured_bps = (uint64_t)pc->bytes_received * 8ULL * 1000000ULL / (uint64_t)span_us;
    } else {
      measured_bps = pc->target_bitrate_bps;
    }
  } else if (pc->packets_received == 1 && pc->packets_lost == 0) {
    measured_bps = pc->target_bitrate_bps;
  }

  bool loss_ok = pc->packets_lost == 0 || (pc->packets_lost * 100 <= pc->packets_sent * 5);
  bool bytes_ok = pc->bytes_received * SFU_PROBE_SUCCESS_THRESHOLD_DEN >= pc->bytes_target * SFU_PROBE_SUCCESS_THRESHOLD_NUM;
  bool rate_ok = measured_bps * SFU_PROBE_SUCCESS_THRESHOLD_DEN >= (uint64_t)pc->target_bitrate_bps * SFU_PROBE_SUCCESS_THRESHOLD_NUM;

  if (loss_ok && bytes_ok && rate_ok) {
    pc->clusters_succeeded++;
    pc->state = SFU_PROBE_STATE_IDLE;
    pc->cooldown_until_us = now_us + SFU_PROBE_COOLDOWN_SUCCESS_US;
    if (out_probe_bitrate_bps) {
      *out_probe_bitrate_bps = pc->target_bitrate_bps;
    }
    if (gcc) {
      gcc_bwe_set_active_probing(gcc, false);
    }
    sfu_metric_inc("congestion_probe_succeeded");
    pthread_mutex_unlock(&pc->lock);
    return true;
  }

  pc->clusters_failed++;
  pc->state = SFU_PROBE_STATE_IDLE;
  pc->cooldown_until_us = now_us + SFU_PROBE_COOLDOWN_FAILURE_US;
  if (gcc) {
    gcc_bwe_set_active_probing(gcc, false);
  }
  sfu_metric_inc("congestion_probe_failed");
  pthread_mutex_unlock(&pc->lock);
  return true;
}

static bool find_outbound_rtx_params(sfu_peer_session_t *s, uint32_t *out_ssrc, uint8_t *out_pt) {
  sfu_receiver_snapshot_t *snapshot = sfu_session_subscriptions_acquire(s);
  if (snapshot) {
    sfu_receiver_snapshot_iter_t iter;
    sfu_receiver_snapshot_iter_init(&iter, snapshot);
    uint32_t slot = 0;
    const sfu_receiver_entry_t *entry;
    while ((entry = sfu_receiver_snapshot_iter_next(&iter, &slot)) != NULL) {
      if (entry->has_screen && entry->screen_rtx_ssrc != 0) {
        *out_ssrc = entry->screen_rtx_ssrc;
        *out_pt = entry->screen_rtx_pt;
        sfu_subscriptions_snapshot_release(snapshot);
        return true;
      }
      if (entry->has_video && entry->video_rtx_ssrc != 0) {
        *out_ssrc = entry->video_rtx_ssrc;
        *out_pt = entry->video_rtx_pt;
        sfu_subscriptions_snapshot_release(snapshot);
        return true;
      }
    }
    sfu_subscriptions_snapshot_release(snapshot);
  }
  if (s->media.screen.rtx_ssrc != 0) {
    *out_ssrc = s->media.screen.rtx_ssrc;
    *out_pt = s->media.screen.rtx_payload_type;
    return true;
  }
  if (s->media.uplink_video.rtx_ssrc != 0) {
    *out_ssrc = s->media.uplink_video.rtx_ssrc;
    *out_pt = s->media.uplink_video.rtx_payload_type;
    return true;
  }
  /* Never fall back to media SSRC — probe padding on media SSRC would corrupt
   * the media sequence space and trigger receiver-side NACK/PLI storms. */
  return false;
}

void sfu_probe_controller_step(sfu_peer_session_t *session, sfu_worker_t *w, int64_t now_us) {
  if (!session || !sfu_session_video_runtime_ready(session) || !session->egress.probe_controller || !session->egress.gcc_ctx) {
    return;
  }
  sfu_probe_controller_t *pc = session->egress.probe_controller;

  uint32_t media_backlog = session->egress.paced_camera.count;
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS && media_backlog == 0; i++) {
    if (session->egress.paced_screen[i].count > 0) {
      media_backlog++;
    }
  }

  pthread_mutex_lock(&pc->lock);
  if (pc->state == SFU_PROBE_STATE_IDLE) {
    pthread_mutex_unlock(&pc->lock);
    bool rtx_available = session->media.screen.rtx_ssrc != 0 || session->media.uplink_video.rtx_ssrc != 0;
    bool should = sfu_probe_controller_should_probe(pc, session->egress.gcc_ctx, now_us,
                                                    session->media.twcc_send_extmap_id != 0,
                                                    rtx_available,
                                                    session->egress.paced_rtx.count, media_backlog);
    if (should) {
      sfu_probe_controller_start_probe(pc, session->egress.gcc_ctx, now_us);
    }
    return;
  }

  if (pc->state == SFU_PROBE_STATE_PROBING) {
    if (session->egress.paced_rtx.count > 0 || media_backlog > 0 || gcc_bwe_is_overusing(session->egress.gcc_ctx)) {
      pthread_mutex_unlock(&pc->lock);
      sfu_probe_controller_abort(pc, session->egress.gcc_ctx, now_us, "backlog_or_overuse");
      return;
    }

    /* PROBING timeout: fail cluster if we couldn't generate anything within a reasonable window. */
    if (pc->start_time_us > 0 && (now_us - pc->start_time_us > SFU_PROBE_CLUSTER_TIMEOUT_US)) {
      pthread_mutex_unlock(&pc->lock);
      uint32_t probe_bps = 0;
      if (sfu_probe_controller_check_cluster_done(pc, session->egress.gcc_ctx, now_us, &probe_bps)) {
        if (probe_bps > 0 && session->egress.gcc_ctx) {
          gcc_bwe_on_probe_cluster_completed(session->egress.gcc_ctx, probe_bps, now_us);
        }
        if (session->egress.gcc_ctx) {
          gcc_bwe_set_active_probing(session->egress.gcc_ctx, false);
        }
      }
      return;
    }

    /* Probe only on a negotiated RTX SSRC to avoid corrupting media sequence space. */
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_pt = 0;
    if (!find_outbound_rtx_params(session, &rtx_ssrc, &rtx_pt)) {
      pthread_mutex_unlock(&pc->lock);
      sfu_probe_controller_abort(pc, session->egress.gcc_ctx, now_us, "no_rtx_params");
      return;
    }

    uint32_t generated = 0;
    while (pc->bytes_sent < pc->bytes_target && generated < SFU_PACED_SEND_MAX_DRAIN_PER_SCAN) {
      if (session->egress.paced_probe.capacity > 0 &&
          session->egress.paced_probe.count >= session->egress.paced_probe.capacity) {
        break;
      }
      uint8_t packet_buf[SFU_PACED_SEND_MAX_PAYLOAD];
      uint16_t rtp_seq = 0;
      if (session->egress.rtx_cache) {
        /* Probes consume RTX sequence numbers but are never cached for retransmission.
         * A receiver NACK for a probe seq would hit a cache miss and trigger a PLI;
         * in practice receivers do not NACK RTX SSRCs, and the alternative (media SSRC)
         * would corrupt media sequencing. */
        uint16_t src_seq = atomic_fetch_add_explicit(&session->egress.rtx_cache->next_rtx_seq, 1, memory_order_relaxed);
        if (!sfu_rtp_seq_translate(&session->cold->rtp_seq_translator, rtx_ssrc, src_seq, &rtp_seq)) {
          rtp_seq = src_seq;
        }
      } else {
        rtp_seq = (uint16_t)(pc->packets_sent + 1);
      }

      uint16_t twcc_seq = atomic_fetch_add_explicit(&session->egress.next_twcc_seq, 1, memory_order_relaxed);
      size_t built_len = 0;
      if (!sfu_rtp_padding_build(packet_buf, sizeof(packet_buf), rtx_pt, rtp_seq, 0, rtx_ssrc,
                                 SFU_RTP_PADDING_DEFAULT_BYTES, session->media.twcc_send_extmap_id,
                                 twcc_seq, &built_len)) {
        break;
      }
      bool twcc_written = session->media.twcc_send_extmap_id != 0;

      pthread_mutex_lock(&session->crypto_lock);
      int enc_len = (int)built_len;
      srtp_err_status_t status = sfu_srtp_protect_rtp_status(&session->srtp, packet_buf, &enc_len, sizeof(packet_buf));
      pthread_mutex_unlock(&session->crypto_lock);
      if (status != srtp_err_status_ok) {
        sfu_metric_inc("egress_protect_fail");
        break;
      }

      uint64_t owner_val = sfu_session_owner_value(session);
      uint32_t trans_gen = atomic_load_explicit(&session->cold->transport_generation, memory_order_acquire);
      uint32_t addr_gen = atomic_load_explicit(&session->cold->address_generation, memory_order_acquire);
      if (!sfu_paced_priority_queue_enqueue_probe(&session->egress.paced_probe, packet_buf, (uint16_t)enc_len,
                                                  &session->cold->addr, session->cold->addr_len,
                                                  owner_val, trans_gen, addr_gen, twcc_written, twcc_seq,
                                                  pc->cluster_id, now_us)) {
        break;
      }
      pc->bytes_sent += (uint32_t)enc_len;
      pc->packets_sent++;
      generated++;
    }

    if (pc->bytes_sent >= pc->bytes_target) {
      pc->state = SFU_PROBE_STATE_WAITING_FEEDBACK;
      pc->send_end_time_us = now_us;
    }
    pthread_mutex_unlock(&pc->lock);
    (void)w;
    return;
  }

  if (pc->state == SFU_PROBE_STATE_WAITING_FEEDBACK) {
    if (pc->send_end_time_us > 0 && (now_us - pc->send_end_time_us > SFU_PROBE_CLUSTER_TIMEOUT_US)) {
      pthread_mutex_unlock(&pc->lock);
      uint32_t probe_bps = 0;
      if (sfu_probe_controller_check_cluster_done(pc, session->egress.gcc_ctx, now_us, &probe_bps)) {
        if (probe_bps > 0 && session->egress.gcc_ctx) {
          gcc_bwe_on_probe_cluster_completed(session->egress.gcc_ctx, probe_bps, now_us);
        }
        if (session->egress.gcc_ctx) {
          gcc_bwe_set_active_probing(session->egress.gcc_ctx, false);
        }
      }
      return;
    }
    pthread_mutex_unlock(&pc->lock);
    return;
  }

  pthread_mutex_unlock(&pc->lock);
}
