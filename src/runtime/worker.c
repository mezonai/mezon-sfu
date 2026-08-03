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

/* Strict RTP header walk via the shared parser. Same accept/reject semantics
 * as the old inline helper (no version check, extension fully bounded), plus
 * padding validation, which is behavior-safe here because VP9 media is never
 * sent padded. Returns payload offset or -1. */
static int sfu_rtp_get_payload_offset(const uint8_t *data, uint32_t len) {
  sfu_rtp_packet_t packet;
  if (!sfu_rtp_packet_parse(data, len, &packet)) {
    return -1;
  }
  return (int)packet.header_len;
}

// Cap on distinct lost sequence numbers serviced per NACK member; bounds work
// per feedback packet and dedups expanded BLP runs.
#define SFU_WORKER_NACK_REQUEST_CAP 48

// Throttle keyframe requests triggered by feedback to one per second per
// session, mirroring the pre-Phase-2 behavior.
#define SFU_WORKER_KF_THROTTLE_MS 1000

static void sfu_svc_update_layers(sfu_peer_session_t *session, uint32_t bitrate_bps) {
  // Example VP9 bitrate ladder
  if (bitrate_bps > 1200000) {
    session->target_sid = 2;  // High resolution (e.g., 720p)
    session->target_tid = 2;  // Full framerate (e.g., 30fps)
  } else if (bitrate_bps > 500000) {
    session->target_sid = 1;  // Medium resolution (e.g., 360p)
    session->target_tid = 2;
  } else if (bitrate_bps > 150000) {
    session->target_sid = 0;  // Low resolution (e.g., 180p)
    session->target_tid = 1;  // Half framerate (e.g., 15fps)
  } else {
    session->target_sid = 0;
    session->target_tid = 0;  // Lowest framerate
  }
}

static void sfu_worker_request_keyframe_throttled(sfu_worker_t *w, sfu_peer_session_t *session) {
  // NOTE: session here is the session the feedback arrived on. True
  // Media-SSRC -> publisher session routing lands with the session/stream
  // lookup rework in the next phase.
  int64_t now = (int64_t)sfu_now_ms();
  if (now - session->last_pli_time > SFU_WORKER_KF_THROTTLE_MS) {
    session->last_pli_time = now;
    sfu_session_request_keyframe(w, session, false);  // false = PLI
  }
}

static void sfu_worker_handle_twcc_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_twcc_parser_t parser;
  // Bound the parser to this member's logical (unpadded) bytes.
  if (sfu_twcc_parser_init(&parser, view->member, view->member_len) != 0) {
    sfu_metric_inc("rtcp_twcc_bad");
    return;
  }

  gcc_packet_info_t twcc_pkt;
  uint32_t estimated_bps = 0;

  if (sender_session->gcc_ctx) {
    estimated_bps = sender_session->gcc_ctx->aimd.current_bitrate_bps;
  }

  while (sfu_twcc_parser_next(&parser, &twcc_pkt)) {
    if (sender_session->twcc_history && sfu_twcc_history_lookup(sender_session->twcc_history, twcc_pkt.sequence_number, &twcc_pkt)) {
      if (sender_session->gcc_ctx) {
        estimated_bps = gcc_bwe_process_twcc_packet(sender_session->gcc_ctx, &twcc_pkt);
      }
    }
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

  // NOTE: lookup stays on the existing per-session RTX cache regardless of
  // the NACK media SSRC; stream-scoped RTX caches are a later phase.
  uint32_t nack_media_ssrc = sfu_nack_parser_media_ssrc(&nack_parser);
  (void)nack_media_ssrc;

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

    uint8_t orig_pkt[SFU_MAX_PAYLOAD_SIZE];
    uint32_t orig_len = 0;
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_pt = 0;

    if (sfu_rtx_cache_get(sender_session->rtx_cache, lost_seq, orig_pkt, &orig_len, &rtx_ssrc, &rtx_pt)) {
      sfu_packet_t *rtx_enc = sfu_packet_pool_alloc(w->pp);
      if (!rtx_enc) {
        continue;
      }

      uint16_t next_rtx_seq = __atomic_fetch_add(&sender_session->rtx_cache->next_rtx_seq, 1, __ATOMIC_RELAXED);
      size_t rtx_built_len = 0;
      // sfu_rtx_build conservatively requires orig_len + 2 bytes of output
      // capacity; the cache only stores packets with len + 2 <=
      // SFU_MAX_PAYLOAD_SIZE <= pool buffer cap, so a successful get always
      // fits. On failure the scratch buffer is untouched.
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
    // On cache miss keep the existing keyframe fallback. Follow-up: route by
    // media SSRC once stream-scoped lookup lands.
    sfu_worker_request_keyframe_throttled(w, sender_session);
  }
}

static void sfu_worker_handle_pli_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_rtcp_pli pli;
  if (!sfu_rtcp_parse_pli(view, &pli)) {
    sfu_metric_inc("rtcp_pli_bad");
    return;
  }
  // NOTE: pli.media_ssrc is parsed and validated, but the keyframe request is
  // still issued via the existing session path; Media-SSRC publisher routing
  // is next phase.
  sfu_worker_request_keyframe_throttled(w, sender_session);
}

static void sfu_worker_handle_fir_member(sfu_worker_t *w, sfu_peer_session_t *sender_session, const sfu_rtcp_member_view *view) {
  sfu_rtcp_fir fir;
  if (!sfu_rtcp_parse_fir(view, &fir)) {
    sfu_metric_inc("rtcp_pli_bad");
    return;
  }
  // Minimal handling: subscribers are not supposed to FIR the SFU; log only.
  SFU_LOG_DEBUG("worker %u: RTCP FIR from peer %u (media_ssrc=%u, %zu entries) ignored", w->worker_index, sender_session->peer_id, fir.media_ssrc,
                fir.entry_count);
}

/* Iterate the (already SRTCP-unprotected) compound RTCP packet and dispatch
 * every valid member. A malformed member drops the remainder of the compound
 * and bumps rtcp_compound_malformed. Consumes pkt; never forwards it. */
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
    return;
  }

  bool is_rtcp = sfu_rtp_is_rtcp(pkt->data, pkt->len);
  int plain_len = (int)pkt->len;
  bool unprotected =
      is_rtcp ? sfu_srtp_unprotect_rtcp(&sender_session->srtp, pkt->data, &plain_len) : sfu_srtp_unprotect_rtp(&sender_session->srtp, pkt->data, &plain_len);

  if (!unprotected) {
    SFU_LOG_WARN("worker %u: [INGRESS DROP] SRTP unprotect FAILED (is_rtcp=%d, len=%u). Key mismatch or corrupted packet!", w->worker_index, is_rtcp, pkt->len);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }
  pkt->len = (uint32_t)plain_len;

  if (is_rtcp) {
    sfu_worker_handle_rtcp(w, sender_session, pkt);
    return;
  }

  bool is_vp9 = false;
  sfu_vp9_descriptor_t vp9_desc = {0};
  bool is_keyframe = false;

  if (!is_rtcp) {
    uint8_t incoming_pt = pkt->data[1] & 0x7F;
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

  sfu_receiver_slot_t **receivers = __atomic_load_n(&sender_session->receivers, __ATOMIC_ACQUIRE);
  uint32_t receiver_capacity = __atomic_load_n(&sender_session->receiver_capacity, __ATOMIC_ACQUIRE);

  for (uint32_t i = 0; i < receiver_capacity; i++) {
    sfu_receiver_slot_t *slot = receivers[i];

    if (!slot || (!slot->video && !slot->audio)) {
      continue;
    }

    sfu_peer_session_t *sub_session = slot->video ? slot->video->owner : slot->audio->owner;
    if (!sub_session || sub_session->state != SFU_SESSION_ESTABLISHED) {
      continue;
    }

    if (!sub_session->scheduler) {
      continue;
    }

    if (sub_session->scheduler->active_publisher_id != sender_session->peer_id) {
      continue;
    }

    if (is_vp9 && slot->video) {
      bool should_forward = sfu_scheduler_evaluate_frame(sub_session->scheduler, &vp9_desc, is_keyframe);
      if (!should_forward) {
        continue;
      }
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
    int enc_len = (int)pkt->len;

    if (!is_rtcp) {
      uint8_t incoming_pt = enc->data[1] & 0x7F;
      uint8_t expected_pt = sfu_session_get_mapped_pt(sub_session, incoming_pt);
      if (incoming_pt != expected_pt) {
        enc->data[1] = (enc->data[1] & 0x80) | (expected_pt & 0x7F);
      }

      uint16_t subscriber_seq = sfu_read_be16(enc->data + 2);
      if (slot->video && incoming_pt == slot->video->payload_type) {
        sfu_rtx_cache_put(sub_session->rtx_cache, subscriber_seq, enc->data, enc_len, slot->video->rtx_ssrc, slot->video->rtx_payload_type);
      }

      uint16_t twcc_seq = __atomic_fetch_add(&sub_session->next_twcc_seq, 1, __ATOMIC_RELAXED);
      int64_t now_ms = sfu_now_ms();
      if (sub_session->twcc_history) {
        sfu_twcc_history_record(sub_session->twcc_history, twcc_seq, now_ms, enc_len);
      }
    }

    bool protected_ = is_rtcp ? sfu_srtp_protect_rtcp(&sub_session->srtp, enc->data, &enc_len, enc->cap)
                              : sfu_srtp_protect_rtp(&sub_session->srtp, enc->data, &enc_len, enc->cap);

    if (!protected_) {
      SFU_LOG_WARN("worker %u: [EGRESS DROP] SRTP protect FAILED", w->worker_index);
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      continue;
    }
    enc->len = (uint32_t)enc_len;

    if (sub_session->worker_id == w->worker_index) {
      if (sfu_ring_queue_send_zc(&w->send_ring, enc, (const struct sockaddr *)&sub_session->cold->addr, sub_session->cold->addr_len) != 0) {
        SFU_LOG_WARN("worker %u: local send SQ full", w->worker_index);
      }
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);  // Packet is now handled by io_uring
    } else {
      if (!sfu_fanout_mesh_enqueue(w->mesh, w->worker_index, sub_session->worker_id, enc, &sub_session->cold->addr, sub_session->cold->addr_len)) {
        SFU_LOG_WARN("worker %u: fanout queue full", w->worker_index);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      }
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
}

static void handle_fanout_job(void *user_data, sfu_fanout_job_t *job) {
  sfu_worker_t *w = (sfu_worker_t *)user_data;

  SFU_LOG_DEBUG("worker %u: [FANOUT DEQUEUE] Sending %u bytes to peer socket via io_uring", w->worker_index, job->pkt->len);

  if (sfu_ring_queue_send_zc(&w->send_ring, job->pkt, (const struct sockaddr *)&job->dst, job->dst_len) != 0) {
    SFU_LOG_WARN("worker %u: [EGRESS DROP] remote-fanout send SQ full, dropping packet", w->worker_index);
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

    unsigned fanned = sfu_fanout_mesh_drain(w->mesh, w->worker_index, SFU_WORKER_REAP_BATCH, handle_fanout_job, w);
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
