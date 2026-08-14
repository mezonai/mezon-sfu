#include "pipeline/router.h"

#include <string.h>

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/egress.h"
#include "runtime/fanout.h"
#include "runtime/worker.h"
#include "util/log.h"

void sfu_router_forward(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m) {
  sfu_packet_t *pkt = m->pkt;

  sfu_receiver_snapshot_t *snap = sfu_session_fanout_targets_acquire(sender_session);
  uint32_t receiver_count = snap ? snap->count : 0;

  for (uint32_t i = 0; i < receiver_count; i++) {
    const sfu_receiver_entry_t *slot = &snap->entries[i];
    if ((m->is_audio && !slot->audio_active) || (!m->is_audio && !slot->video_active)) {
      continue;
    }

    sfu_peer_session_t *sub_session = slot->subscriber;
    if (!sub_session || sub_session->state != SFU_SESSION_ESTABLISHED) {
      continue;
    }

    if (!m->is_audio && !atomic_load_explicit(&sub_session->visible, memory_order_acquire)) {
      continue;
    }

    sfu_packet_t *enc = sfu_packet_pool_alloc(w->pp);
    if (!enc) {
      SFU_LOG_WARN("worker %u: packet pool exhausted, dropping frame for all subscribers", w->worker_index);
      continue;
    }
    if (pkt->len > enc->cap) {
      SFU_LOG_WARN("worker %u: plaintext too large", w->worker_index);
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      continue;
    }

    memcpy(enc->data, pkt->data, pkt->len);
    enc->len = pkt->len;

    sfu_egress_media_t media = {
        .publisher = sender_session,
        .video_ssrc = slot->video_ssrc,
        .video_rtx_ssrc = slot->video_rtx_ssrc,
        .video_pt = slot->video_pt,
        .video_rtx_pt = slot->video_rtx_pt,
        .has_video = slot->has_video,
        .is_audio = m->is_audio,
        .has_svc = m->has_svc,
        .is_keyframe = m->is_keyframe,
    };
    if (m->has_svc) {
      media.svc = m->svc;
    }

    uint16_t owner_worker = sfu_session_owner_worker(sub_session);
    if (owner_worker == w->worker_index) {
      (void)sfu_egress_process(w, sub_session, enc, &sub_session->cold->addr, sub_session->cold->addr_len, &media);
      continue;
    }

    atomic_fetch_add_explicit(&sub_session->refcount, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&sender_session->refcount, 1, memory_order_relaxed);
    if (!sfu_fanout_mesh_enqueue_forward(w->mesh, w->worker_index, owner_worker, enc, sub_session, sender_session, &sub_session->cold->addr,
                                         sub_session->cold->addr_len, slot->video_ssrc, slot->video_rtx_ssrc, slot->video_pt, slot->video_rtx_pt,
                                         slot->has_video, m->is_audio, m->has_svc ? &m->svc : NULL, m->has_svc, m->is_keyframe)) {
      SFU_LOG_WARN("worker %u: fanout queue full", w->worker_index);
      sfu_session_release(sender_session);
      sfu_session_release(sub_session);
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
    }
  }

  sfu_subscriptions_snapshot_release(snap);
  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
}
