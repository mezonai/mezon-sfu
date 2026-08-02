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
#include "runtime/cpu.h"
#include "runtime/scheduler.h"
#include "runtime/signal.h"
#include "runtime/timer.h"
#include "transport/srtp/srtp.h"
#include "util/log.h"

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
  if (len < 12) {
    return -1;
  }
  uint8_t csrc_count = data[0] & 0x0F;
  uint32_t offset = 12 + (csrc_count * 4);

  if (len < offset) {
    return -1;
  }

  if (data[0] & 0x10) {
    if (len < offset + 4) {
      return -1;
    }
    uint16_t ext_len = (data[offset + 2] << 8) | data[offset + 3];
    offset += 4 + (ext_len * 4);
  }

  return (offset <= len) ? (int)offset : -1;
}

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

  if (is_rtcp && pkt->len >= 8) {
    uint8_t fmt = pkt->data[0] & 0x1F;
    uint8_t pt = pkt->data[1];

    if (pt == 205 && fmt == 15) {  // Transport-Wide Feedback
      sfu_twcc_parser_t parser;

      if (sfu_twcc_parser_init(&parser, pkt->data, pkt->len) == 0) {
        gcc_packet_info_t twcc_pkt;
        uint32_t estimated_bps = sender_session->gcc_ctx->aimd.current_bitrate_bps;

        // Iterate through status chunks in feedback packet
        while (sfu_twcc_parser_next(&parser, &twcc_pkt)) {
          // Lookup local send history using parsed sequence number
          if (sfu_twcc_history_lookup(sender_session->twcc_history, twcc_pkt.sequence_number, &twcc_pkt)) {
            // Feed completely populated packet info into GCC BWE engine
            estimated_bps = gcc_bwe_process_twcc_packet(sender_session->gcc_ctx, &twcc_pkt);
          }
        }

        // Adapt active dynamic target layers
        sfu_svc_update_layers(sender_session, estimated_bps);
      }

      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
      return;
    }
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
          // Identify if this is a sequence Keyframe (Non-predicted, Base layer)
          is_keyframe = (vp9_desc.p_bit == 0 && vp9_desc.sid == 0);
        } else {
          is_vp9 = false;  // Corrupted payload
        }
      }
    }
  }

  sfu_receiver_slot_t **receivers = __atomic_load_n(&sender_session->receivers, __ATOMIC_ACQUIRE);
  uint32_t receiver_capacity = __atomic_load_n(&sender_session->receiver_capacity, __ATOMIC_ACQUIRE);

  for (uint32_t i = 0; i < receiver_capacity; i++) {
    sfu_receiver_slot_t *slot = receivers[i];

    if (!slot) {
      continue;
    }

    if (!slot->video && !slot->audio) {
      continue;
    }

    sfu_peer_session_t *sub_session = slot->video ? slot->video->owner : slot->audio->owner;
    if (!sub_session || sub_session->state != SFU_SESSION_ESTABLISHED) {
      continue;
    }

    // Routing Check: Is this subscriber pinned to or actively watching THIS publisher?
    if (sub_session->scheduler->active_publisher_id != sender_session->peer_id) {
      continue;
    }

    // Layer Evaluation
    if (is_vp9 && slot->video) {
      bool should_forward = sfu_scheduler_evaluate_frame(sub_session->scheduler, &vp9_desc, is_keyframe);

      if (!should_forward) {
        continue;  // Scheduler rejected the frame (wrong layer, waiting for keyframe, etc.)
      }
    }

    sfu_packet_t *enc = sfu_packet_pool_alloc(w->pp);
    if (!enc) {
      SFU_LOG_WARN("worker %u: packet pool exhausted, dropping subscriber send", w->worker_index);
      continue;
    }
    if (pkt->len > enc->cap) {
      SFU_LOG_WARN("worker %u: plaintext too large to re-encrypt (%u > %u)", w->worker_index, pkt->len, enc->cap);
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
    }

    bool protected_ = is_rtcp ? sfu_srtp_protect_rtcp(&sub_session->srtp, enc->data, &enc_len, enc->cap)
                              : sfu_srtp_protect_rtp(&sub_session->srtp, enc->data, &enc_len, enc->cap);

    if (!protected_) {
      SFU_LOG_WARN("worker %u: [EGRESS DROP SUB %u] SRTP protect FAILED for target worker %u!", w->worker_index, i, sub_session->worker_id);
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      continue;
    }
    enc->len = (uint32_t)enc_len;

    SFU_LOG_DEBUG("worker fwd from %u to %u (len=%u)", w->worker_index, sub_session->worker_id, enc->len);

    if (sub_session->worker_id == w->worker_index) {
      if (sfu_ring_queue_send_zc(&w->send_ring, enc, (const struct sockaddr *)&sub_session->cold->addr, sub_session->cold->addr_len) != 0) {
        SFU_LOG_WARN("worker %u: local send SQ full, dropping to subscriber", w->worker_index);
      }
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
    } else {
      if (!sfu_fanout_mesh_enqueue(w->mesh, w->worker_index, sub_session->worker_id, enc, &sub_session->cold->addr, sub_session->cold->addr_len)) {
        SFU_LOG_WARN("worker %u: [EGRESS DROP SUB %u] fanout_mesh_enqueue failed (queue full?) to worker %u", w->worker_index, i, sub_session->worker_id);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      }
    }

    if (!is_rtcp) {
      uint16_t twcc_seq = __atomic_fetch_add(&sub_session->next_twcc_seq, 1, __ATOMIC_RELAXED);
      int64_t now_ms = sfu_now_ms();
      sfu_twcc_history_record(sub_session->twcc_history, twcc_seq, now_ms, enc->len);
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
