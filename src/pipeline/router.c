#include "pipeline/router.h"

#include <string.h>

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/egress.h"
#include "pipeline/keyframe.h"
#include "runtime/scheduler.h"
#include "runtime/worker.h"
#include "util/log.h"

void sfu_router_forward(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m) {
  sfu_packet_t *pkt = m->pkt;

  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(sender_session);
  uint32_t receiver_count = snap ? snap->count : 0;

  for (uint32_t i = 0; i < receiver_count; i++) {
    const sfu_receiver_entry_t *slot = &snap->entries[i];

    sfu_peer_session_t *sub_session = slot->subscriber;
    if (!sub_session || sub_session->state != SFU_SESSION_ESTABLISHED) {
      continue;
    }

    if (!sub_session->schedulers) {
      continue;
    }

    sfu_subscriber_scheduler_t *sched = sfu_session_scheduler_for(sub_session, sender_session->peer_id);
    if (!sched) {
      continue;
    }

    sfu_pacer_class_t video_class = SFU_PACER_CLASS_VIDEO_BASE;
    if (m->has_svc && slot->has_video) {
      if (sched->needs_keyframe) {
        sfu_worker_request_keyframe_throttled(w, sender_session);
      }

      bool should_forward = sfu_scheduler_evaluate_frame(sched, &m->svc, m->is_keyframe);
      if (!should_forward) {
        continue;
      }
      video_class = sfu_scheduler_classify_frame(sched, &m->svc);
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

    sfu_egress_process(w, sub_session, enc, &sub_session->cold->addr, sub_session->cold->addr_len, slot->video_ssrc, slot->video_pt, slot->video_rtx_pt,
                       slot->video_rtx_ssrc, slot->has_video, m->is_audio, video_class);
  }

  sfu_subscriptions_snapshot_release(snap);
  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
}
