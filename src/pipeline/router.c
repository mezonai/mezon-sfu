#include "pipeline/router.h"

#include <string.h>

#include "memory/packet_pool.h"
#include "memory/refcount.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/egress.h"
#include "runtime/fanout.h"
#include "runtime/worker.h"
#include "util/metrics.h"

typedef struct {
  sfu_fanout_target_t targets[SFU_FANOUT_BATCH_CAP];
  uint8_t count;
} sfu_route_batch_builder_t;

static void forward_local(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m, sfu_peer_session_t *sub_session, uint32_t video_ssrc,
                          uint32_t video_rtx_ssrc, uint32_t remote_slot, uint64_t assignment_generation, uint8_t video_pt, uint8_t video_rtx_pt, bool has_video) {
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
  (void)sfu_egress_process_plaintext(w, sub_session, m->pkt, &sub_session->cold->addr, sub_session->cold->addr_len, &media);
}

static bool ensure_remote_source(sfu_worker_t *w, const sfu_packet_t *plain, sfu_packet_t **source) {
  if (*source) {
    return true;
  }
  sfu_packet_t *copy = sfu_packet_pool_alloc(w->pp);
  if (!copy || plain->len > copy->cap) {
    if (copy) {
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, copy);
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
                               sfu_route_batch_builder_t *builder, sfu_packet_t **remote_source) {
  if (builder->count == 0) {
    return;
  }
  if (!ensure_remote_source(w, m->pkt, remote_source)) {
    builder->count = 0;
    return;
  }

  sfu_packet_retain(*remote_source, 1);
  sfu_peer_session_t *publisher = m->is_audio ? NULL : sender_session;
  if (!sfu_fanout_mesh_enqueue_forward_batch(w->mesh, w->worker_index, dst_worker, *remote_source, publisher, builder->targets, builder->count, m->is_audio,
                                             m->has_svc ? &m->svc : NULL, m->has_svc, m->is_keyframe)) {
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, *remote_source);
  }
  builder->count = 0;
}

static void route_target(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m, sfu_peer_session_t *subscriber, uint32_t video_ssrc,
                         uint32_t video_rtx_ssrc, uint32_t remote_slot, uint64_t assignment_generation, uint8_t video_pt, uint8_t video_rtx_pt, bool has_video,
                         sfu_route_batch_builder_t builders[SFU_MAX_WORKERS], sfu_packet_t **remote_source) {
  if (!subscriber || subscriber->state != SFU_SESSION_ESTABLISHED || !sfu_session_accepts_work(subscriber)) {
    return;
  }
  if (!m->is_audio && !atomic_load_explicit(&subscriber->media.visible, memory_order_acquire)) {
    return;
  }

  uint16_t owner_worker = sfu_session_owner_worker(subscriber);
  uint32_t worker_count = w->mesh ? w->mesh->worker_count : 1;
  if (owner_worker == w->worker_index) {
    forward_local(w, sender_session, m, subscriber, video_ssrc, video_rtx_ssrc, remote_slot, assignment_generation, video_pt, video_rtx_pt, has_video);
    return;
  }
  if (owner_worker == SFU_SESSION_OWNER_NONE || owner_worker >= worker_count) {
    return;
  }

  sfu_route_batch_builder_t *builder = &builders[owner_worker];
  if (builder->count == SFU_FANOUT_BATCH_CAP) {
    flush_remote_batch(w, sender_session, m, owner_worker, builder, remote_source);
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
  while ((entry = sfu_fanout_iter_next(&iter, NULL)) != NULL) {
    if (kind == SFU_MEDIA_AUDIO) {
      route_target(w, sender_session, m, entry->subscriber, 0, 0, entry->remote_slot, entry->assignment_generation, 0, 0, false, builders, &remote_source);
    } else if (kind == SFU_MEDIA_SCREEN) {
      route_target(w, sender_session, m, entry->subscriber, entry->screen_ssrc, entry->screen_rtx_ssrc, entry->remote_slot, entry->assignment_generation,
                   entry->screen_pt, entry->screen_rtx_pt, true, builders, &remote_source);
    } else {
      route_target(w, sender_session, m, entry->subscriber, entry->video_ssrc, entry->video_rtx_ssrc, entry->remote_slot, entry->assignment_generation,
                   entry->video_pt, entry->video_rtx_pt, true, builders, &remote_source);
    }
  }
  for (uint32_t dst = 0; dst < worker_count; dst++) {
    if (dst != w->worker_index) {
      flush_remote_batch(w, sender_session, m, dst, &builders[dst], &remote_source);
    }
  }
  sfu_fanout_bundle_release(bundle);
  if (remote_source) {
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, remote_source);
  }
  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, m->pkt);
}
