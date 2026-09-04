#include "pipeline/router.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "memory/packet_pool.h"
#include "memory/refcount.h"
#include "peer/session.h"
#include "pipeline/egress.h"
#include "runtime/fanout.h"
#include "runtime/worker.h"
#include "util/log.h"
#include "util/metrics.h"
#include "util/netbytes.h"

typedef struct {
  sfu_fanout_target_t targets[SFU_FANOUT_BATCH_CAP];
  uint8_t count;
} sfu_route_batch_builder_t;

#ifdef SFU_DIAG_LOG
typedef struct {
  uint32_t recipients, eligible, ineligible, invisible, invalid_owner;
  uint32_t local, local_ok, local_fail, remote_targets;
  uint32_t batches, enqueued, enqueue_fail, copy_fail, generation_zero;
  uint64_t generation_min, generation_max;
} sfu_vp9_dispatch_diag_t;

static void dispatch_record_generation(sfu_vp9_dispatch_diag_t *diag, uint64_t generation) {
  if (!diag) {
    return;
  }
  if (generation == 0) {
    diag->generation_zero++;
  }
  if (diag->generation_min == 0 || generation < diag->generation_min) {
    diag->generation_min = generation;
  }
  if (generation > diag->generation_max) {
    diag->generation_max = generation;
  }
}
#endif

static bool forward_local(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m, sfu_peer_session_t *sub_session, uint32_t video_ssrc,
                          uint32_t video_rtx_ssrc, uint32_t remote_slot, uint64_t assignment_generation, uint8_t video_pt, uint8_t video_rtx_pt,
                          bool has_video) {
  sfu_egress_media_t media = {
      .publisher = sender_session,
      .source = m->source,
      .video_ssrc = video_ssrc,
      .video_rtx_ssrc = video_rtx_ssrc,
      .remote_slot = remote_slot,
      .assignment_generation = assignment_generation,
      .video_pt = video_pt,
      .video_rtx_pt = video_rtx_pt,
      .has_video = has_video,
      .is_audio = m->is_audio,
      .has_svc = m->has_svc,
      .is_keyframe = m->is_keyframe,
  };
  if (m->has_svc) {
    media.svc = m->svc;
  }
  return sfu_egress_process_plaintext(w, sub_session, m->pkt, &sub_session->cold->addr, sub_session->cold->addr_len, &media);
}

static bool ensure_remote_source(sfu_worker_t *w, const sfu_packet_t *plain, sfu_packet_t **source) {
  if (*source) {
    return true;
  }
  sfu_packet_t *copy = sfu_packet_pool_alloc(w->pp);
  if (!copy || plain->len > copy->cap) {
    if (copy) {
      sfu_worker_release_packet(w, copy);
    }
    return false;
  }
  memcpy(copy->data, plain->data, plain->len);
  copy->len = plain->len;
  sfu_metric_inc("egress_output_alloc");
  sfu_metric_add("egress_copied_bytes", plain->len);
  *source = copy;
  return true;
}

static void flush_remote_batch(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m, uint32_t dst_worker,
                               sfu_route_batch_builder_t *builder, sfu_packet_t **remote_source
#ifdef SFU_DIAG_LOG
                               ,
                               sfu_vp9_dispatch_diag_t *diag
#endif
) {
  if (builder->count == 0) {
    return;
  }
#ifdef SFU_DIAG_LOG
  if (diag) {
    diag->batches++;
  }
#endif
  if (!ensure_remote_source(w, m->pkt, remote_source)) {
#ifdef SFU_DIAG_LOG
    if (diag) {
      diag->copy_fail++;
    }
#endif
    builder->count = 0;
    return;
  }
  sfu_packet_retain(*remote_source, 1);
  sfu_peer_session_t *publisher = m->is_audio ? NULL : sender_session;
  bool enqueued = sfu_fanout_mesh_enqueue_forward_batch(w->mesh, w->worker_index, dst_worker, *remote_source, publisher, builder->targets, builder->count,
                                                        m->is_audio, m->has_svc ? &m->svc : NULL, m->has_svc, m->is_keyframe);
#ifdef SFU_DIAG_LOG
  if (diag) {
    if (enqueued) {
      diag->enqueued++;
    } else {
      diag->enqueue_fail++;
    }
  }
#endif
  if (!enqueued) {
    sfu_worker_release_packet(w, *remote_source);
  }
  builder->count = 0;
}

static void route_target(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m, sfu_peer_session_t *subscriber, uint32_t video_ssrc,
                         uint32_t video_rtx_ssrc, uint32_t remote_slot, uint64_t assignment_generation, uint8_t video_pt, uint8_t video_rtx_pt, bool has_video,
                         sfu_route_batch_builder_t builders[SFU_MAX_WORKERS], sfu_packet_t **remote_source
#ifdef SFU_DIAG_LOG
                         ,
                         sfu_vp9_dispatch_diag_t *diag
#endif
) {
#ifdef SFU_DIAG_LOG
  dispatch_record_generation(diag, assignment_generation);
#endif
  if (!subscriber || subscriber->state != SFU_SESSION_ESTABLISHED || !sfu_session_accepts_work(subscriber)) {
#ifdef SFU_DIAG_LOG
    if (diag) {
      diag->ineligible++;
    }
#endif
    return;
  }
  if (!m->is_audio && !atomic_load_explicit(&subscriber->media.visible, memory_order_acquire)) {
#ifdef SFU_DIAG_LOG
    if (diag) {
      diag->invisible++;
    }
#endif
    return;
  }
#ifdef SFU_DIAG_LOG
  if (diag) {
    diag->eligible++;
  }
#endif
  uint16_t owner_worker = sfu_session_owner_worker(subscriber);
  uint32_t worker_count = w->mesh ? w->mesh->worker_count : 1;
  if (owner_worker == w->worker_index) {
#ifdef SFU_DIAG_LOG
    bool ok =
        forward_local(w, sender_session, m, subscriber, video_ssrc, video_rtx_ssrc, remote_slot, assignment_generation, video_pt, video_rtx_pt, has_video);
    if (diag) {
      diag->local++;
      ok ? diag->local_ok++ : diag->local_fail++;
    }
#else
    (void)forward_local(w, sender_session, m, subscriber, video_ssrc, video_rtx_ssrc, remote_slot, assignment_generation, video_pt, video_rtx_pt, has_video);
#endif
    return;
  }
  if (owner_worker == SFU_SESSION_OWNER_NONE || owner_worker >= worker_count) {
#ifdef SFU_DIAG_LOG
    if (diag) {
      diag->invalid_owner++;
    }
#endif
    return;
  }
  sfu_route_batch_builder_t *builder = &builders[owner_worker];
  if (builder->count == SFU_FANOUT_BATCH_CAP) {
    flush_remote_batch(w, sender_session, m, owner_worker, builder, remote_source
#ifdef SFU_DIAG_LOG
                       ,
                       diag
#endif
    );
  }
  sfu_fanout_target_t *target = &builder->targets[builder->count++];
  memset(target, 0, sizeof(*target));
  target->subscriber = subscriber;
  target->dst = subscriber->cold->addr;
  target->dst_len = subscriber->cold->addr_len;
  target->video_ssrc = video_ssrc;
  target->video_rtx_ssrc = video_rtx_ssrc;
  target->remote_slot = remote_slot;
  target->assignment_generation = assignment_generation;
  target->video_pt = video_pt;
  target->video_rtx_pt = video_rtx_pt;
  target->source = (uint8_t)m->source;
  target->has_video = has_video;
#ifdef SFU_DIAG_LOG
  if (diag) {
    diag->remote_targets++;
  }
#endif
}

void sfu_router_forward(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m) {
  uint32_t worker_count = w->mesh ? w->mesh->worker_count : 1;
  sfu_route_batch_builder_t builders[SFU_MAX_WORKERS];
  memset(builders, 0, sizeof(builders));
  sfu_packet_t *remote_source = NULL;
  sfu_media_kind_t kind = m->is_audio ? SFU_MEDIA_AUDIO : m->source == SFU_MEDIA_SCREEN ? SFU_MEDIA_SCREEN : SFU_MEDIA_VIDEO;
  sfu_fanout_bundle_t *bundle = sfu_session_fanout_acquire(sender_session);
  sfu_fanout_iter_t iter;
  sfu_fanout_iter_init(&iter, bundle, kind);
  const sfu_fanout_route_t *entry;
#ifdef SFU_DIAG_LOG
  uint32_t audio_dispatched = 0;
  uint32_t audio_skipped_pending = 0;
  uint32_t routed = 0;
  static _Atomic uint32_t dispatch_logs;
  uint32_t dispatch_n = 0;
  bool log_dispatch = false;
  sfu_vp9_dispatch_diag_t diag = {0};
  if (kind == SFU_MEDIA_SCREEN && m->has_svc && m->svc.b_bit != 0) {
    dispatch_n = atomic_fetch_add_explicit(&dispatch_logs, 1, memory_order_relaxed);
    log_dispatch = dispatch_n == 0 || (dispatch_n & 127u) == 0;
  }
#endif
  while ((entry = sfu_fanout_iter_next(&iter, NULL)) != NULL) {
#ifdef SFU_DIAG_LOG
    routed++;
#endif
    if (!sfu_session_remote_slot_authorized(entry->subscriber, entry->remote_slot, entry->assignment_generation)) {
      sfu_metric_inc("router_assignment_pending");
#ifdef SFU_DIAG_LOG
      if (kind == SFU_MEDIA_AUDIO) {
        audio_skipped_pending++;
      }
      if (log_dispatch) {
        diag.generation_zero++;
      }
#endif
      continue;
    }
#ifdef SFU_DIAG_LOG
    if (kind == SFU_MEDIA_AUDIO) {
      audio_dispatched++;
    }
    if (log_dispatch) {
      diag.recipients++;
    }
#endif
    if (kind == SFU_MEDIA_AUDIO) {
      route_target(w, sender_session, m, entry->subscriber, 0, 0, entry->remote_slot, entry->assignment_generation, 0, 0, false, builders, &remote_source
#ifdef SFU_DIAG_LOG
                   ,
                   NULL
#endif
      );
    } else if (kind == SFU_MEDIA_SCREEN) {
      route_target(w, sender_session, m, entry->subscriber, entry->screen_ssrc, entry->screen_rtx_ssrc, entry->remote_slot, entry->assignment_generation,
                   entry->screen_pt, entry->screen_rtx_pt, true, builders, &remote_source
#ifdef SFU_DIAG_LOG
                   ,
                   log_dispatch ? &diag : NULL
#endif
      );
    } else {
      route_target(w, sender_session, m, entry->subscriber, entry->video_ssrc, entry->video_rtx_ssrc, entry->remote_slot, entry->assignment_generation,
                   entry->video_pt, entry->video_rtx_pt, true, builders, &remote_source
#ifdef SFU_DIAG_LOG
                   ,
                   NULL
#endif
      );
    }
  }
  for (uint32_t dst = 0; dst < worker_count; dst++) {
    if (dst != w->worker_index) {
      flush_remote_batch(w, sender_session, m, dst, &builders[dst], &remote_source
#ifdef SFU_DIAG_LOG
                         ,
                         log_dispatch ? &diag : NULL
#endif
      );
    }
  }
#ifdef SFU_DIAG_LOG
  if (log_dispatch) {
    uint16_t seq = m->pkt->len >= 4 ? sfu_read_be16(m->pkt->data + 2) : 0;
    uint32_t ts = m->pkt->len >= 8 ? sfu_read_be32(m->pkt->data + 4) : m->svc.rtp_timestamp;
    uint32_t ssrc = m->pkt->len >= 12 ? sfu_read_be32(m->pkt->data + 8) : 0;
    SFU_LOG_WARN("[VP9-DBG] dispatch n=%u worker=%u pub=%u ssrc=%" PRIu32 " seq=%u ts=%" PRIu32
                 " sid=%u tid=%u kf=%u recipients=%u eligible=%u ineligible=%u invisible=%u invalid_owner=%u local=%u local_ok=%u local_fail=%u remote=%u "
                 "batches=%u enqueued=%u enqueue_fail=%u copy_fail=%u gen_zero=%u gen=%" PRIu64 "-%" PRIu64,
                 dispatch_n + 1, w->worker_index, sender_session->peer_id, ssrc, seq, ts, m->svc.sid, m->svc.tid, m->is_keyframe, diag.recipients,
                 diag.eligible, diag.ineligible, diag.invisible, diag.invalid_owner, diag.local, diag.local_ok, diag.local_fail, diag.remote_targets,
                 diag.batches, diag.enqueued, diag.enqueue_fail, diag.copy_fail, diag.generation_zero, diag.generation_min, diag.generation_max);
  }
  if (routed == 0) {
    static _Atomic uint32_t empty_fanout_logs;
    uint32_t n = atomic_fetch_add_explicit(&empty_fanout_logs, 1, memory_order_relaxed);
    if (n == 0 || (n & 127u) == 0) {
      uint32_t stored = bundle ? bundle->count : 0;
      SFU_LOG_WARN("router: empty fanout n=%u peer=%u kind=%d stored=%u", n + 1, sender_session->peer_id, (int)kind, stored);
    }
  }

  if (kind == SFU_MEDIA_AUDIO) {
    if (audio_dispatched > 0) {
      atomic_fetch_add_explicit(&sender_session->media.ptt_diag.route_dispatches, 1, memory_order_relaxed);
    } else if (audio_skipped_pending > 0) {
      atomic_fetch_add_explicit(&sender_session->media.ptt_diag.router_pending_skips, 1, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&sender_session->media.ptt_diag.empty_fanout, 1, memory_order_relaxed);
    }
  }
#endif
  sfu_fanout_bundle_release(bundle);
  if (remote_source) {
    sfu_worker_release_packet(w, remote_source);
  }
  sfu_worker_release_packet(w, m->pkt);
}
