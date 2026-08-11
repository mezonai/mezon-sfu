#include "runtime/fanout_job.h"

#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/egress.h"
#include "runtime/worker.h"
#include "util/log.h"

void sfu_worker_handle_fanout_job(void *user_data, sfu_fanout_job_t *job) {
  sfu_worker_t *w = (sfu_worker_t *)user_data;

  if (job->kind == SFU_FANOUT_JOB_KEYFRAME_REQUEST) {
    if (job->publisher) {
      sfu_session_request_keyframe(w, job->publisher, false);
      sfu_session_release(job->publisher);
    }
    sfu_fanout_mesh_free_job(w->mesh, job);
    return;
  } else if (job->kind == SFU_FANOUT_JOB_FORWARD && job->subscriber) {
    sfu_egress_media_t media = {
        .publisher = job->publisher,
        .svc = job->svc,
        .video_ssrc = job->video_ssrc,
        .video_rtx_ssrc = job->video_rtx_ssrc,
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
