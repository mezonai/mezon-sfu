#include "runtime/fanout_job.h"

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/egress.h"
#include "runtime/worker.h"
#include "util/log.h"
#include "util/metrics.h"

#include <string.h>

void sfu_worker_handle_fanout_job(void *user_data, sfu_fanout_job_t *job) {
  sfu_worker_t *w = (sfu_worker_t *)user_data;

  if (job->kind == SFU_FANOUT_JOB_BATCH) {
    for (uint8_t i = 0; i < job->target_count; i++) {
      sfu_fanout_target_t *target = &job->targets[i];
      sfu_peer_session_t *subscriber = target->subscriber;
      if (!subscriber || sfu_session_owner_worker(subscriber) != w->worker_index || !sfu_session_accepts_work(subscriber)) {
        if (subscriber) {
          sfu_session_release(subscriber);
        }
        continue;
      }

      sfu_egress_media_t media = {
          .publisher = job->publisher,
          .svc = job->svc,
          .source = (sfu_media_kind_t)target->source,
          .video_ssrc = target->video_ssrc,
          .video_rtx_ssrc = target->video_rtx_ssrc,
          .mid = target->mid,
          .video_pt = target->video_pt,
          .video_rtx_pt = target->video_rtx_pt,
          .has_video = target->has_video,
          .is_audio = job->is_audio,
          .has_svc = job->has_svc,
          .is_keyframe = job->is_keyframe,
      };
      (void)sfu_egress_process_plaintext(w, subscriber, job->pkt, &target->dst, target->dst_len, &media);
      sfu_session_release(subscriber);
    }
    if (job->publisher) {
      sfu_session_release(job->publisher);
    }
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, job->pkt);
    sfu_fanout_mesh_free_job(w->mesh, job);
    return;
  }

  if (job->kind == SFU_FANOUT_JOB_KEYFRAME_REQUEST) {
    if (job->publisher) {
      sfu_session_request_keyframe_for_source(w, job->publisher, false, (sfu_media_kind_t)job->source);
      sfu_session_release(job->publisher);
    }
    sfu_fanout_mesh_free_job(w->mesh, job);
    return;
  } else if (job->kind == SFU_FANOUT_JOB_FORWARD && job->subscriber) {
    sfu_egress_media_t media = {
        .publisher = job->publisher,
        .svc = job->svc,
        .source = (sfu_media_kind_t)job->source,
        .video_ssrc = job->video_ssrc,
        .video_rtx_ssrc = job->video_rtx_ssrc,
        .mid = job->mid,
        .video_pt = job->video_pt,
        .video_rtx_pt = job->video_rtx_pt,
        .has_video = job->has_video,
        .is_audio = job->is_audio,
        .has_svc = job->has_svc,
        .is_keyframe = job->is_keyframe,
    };
    (void)sfu_egress_process(w, job->subscriber, job->pkt, &job->dst, job->dst_len, &media);
    if (job->publisher) {
      sfu_session_release(job->publisher);
    }
    sfu_session_release(job->subscriber);
    sfu_fanout_mesh_free_job(w->mesh, job);
    return;
  } else {
    if (sfu_ring_queue_send_zc(&w->send_ring, job->pkt, (const struct sockaddr *)&job->dst, job->dst_len) != 0) {
      SFU_LOG_WARN("worker %u: [EGRESS DROP] remote-fanout send SQ full, dropping packet", w->worker_index);
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, job->pkt);
  sfu_fanout_mesh_free_job(w->mesh, job);
}
