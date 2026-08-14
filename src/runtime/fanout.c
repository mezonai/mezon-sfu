#include "runtime/fanout.h"
#include "peer/session.h"
#include "sfu/config.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/metrics.h"

#include <stddef.h>
#include <string.h>

static uint32_t partition_capacity(uint32_t job_pool_capacity, uint32_t worker_count) {
  uint32_t cap = job_pool_capacity / worker_count;
  return cap > 0 ? cap : 1;
}

int sfu_fanout_mesh_init(sfu_fanout_mesh_t *mesh, uint32_t worker_count, uint32_t ring_capacity, uint32_t job_pool_capacity) {
  memset(mesh, 0, sizeof(*mesh));

  if (worker_count < 1 || worker_count > SFU_MAX_WORKERS) {
    SFU_LOG_ERROR("fanout mesh: invalid worker_count %u (must be 1..%d)", worker_count, SFU_MAX_WORKERS);
    return -1;
  }

  mesh->worker_count = worker_count;
  mesh->per_partition_capacity = partition_capacity(job_pool_capacity, worker_count);

  mesh->job_pools = SFU_CALLOC(worker_count, sizeof(sfu_pool_t));
  if (!mesh->job_pools) {
    SFU_LOG_ERROR("fanout mesh: failed to allocate job pool partitions");
    return -1;
  }
  for (uint32_t i = 0; i < worker_count; i++) {
    if (sfu_pool_init(&mesh->job_pools[i], mesh->per_partition_capacity, sizeof(sfu_fanout_job_t)) != 0) {
      SFU_LOG_ERROR("fanout mesh: failed to init job pool partition %u", i);
      for (uint32_t j = 0; j < i; j++) {
        sfu_pool_destroy(&mesh->job_pools[j]);
      }
      SFU_FREE(mesh->job_pools);
      mesh->job_pools = NULL;
      return -1;
    }
  }

  uint32_t cell_count = worker_count * worker_count;
  mesh->rings = SFU_CALLOC(cell_count, sizeof(sfu_spsc_ring_t));
  if (!mesh->rings) {
    SFU_LOG_ERROR("fanout mesh: failed to allocate ring array");
    for (uint32_t i = 0; i < worker_count; i++) {
      sfu_pool_destroy(&mesh->job_pools[i]);
    }
    SFU_FREE(mesh->job_pools);
    mesh->job_pools = NULL;
    return -1;
  }

  for (uint32_t i = 0; i < cell_count; i++) {
    if (sfu_spsc_ring_init(&mesh->rings[i], ring_capacity) != 0) {
      SFU_LOG_ERROR("fanout mesh: failed to init ring %u", i);
      for (uint32_t j = 0; j < i; j++) {
        sfu_spsc_ring_destroy(&mesh->rings[j]);
      }
      SFU_FREE(mesh->rings);
      mesh->rings = NULL;
      for (uint32_t j = 0; j < worker_count; j++) {
        sfu_pool_destroy(&mesh->job_pools[j]);
      }
      SFU_FREE(mesh->job_pools);
      mesh->job_pools = NULL;
      return -1;
    }
  }

  SFU_LOG_INFO("fanout mesh initialized: %u workers, %u rings, %u job slots per destination", worker_count, cell_count, mesh->per_partition_capacity);
  return 0;
}

void sfu_fanout_mesh_destroy(sfu_fanout_mesh_t *mesh) {
  uint32_t cell_count = mesh->worker_count * mesh->worker_count;
  for (uint32_t i = 0; i < cell_count; i++) {
    sfu_spsc_ring_destroy(&mesh->rings[i]);
  }
  SFU_FREE(mesh->rings);
  if (mesh->job_pools) {
    for (uint32_t i = 0; i < mesh->worker_count; i++) {
      sfu_pool_destroy(&mesh->job_pools[i]);
    }
    SFU_FREE(mesh->job_pools);
  }
}

static sfu_spsc_ring_t *mesh_ring(sfu_fanout_mesh_t *mesh, uint32_t src, uint32_t dst) { return &mesh->rings[src * mesh->worker_count + dst]; }

static sfu_fanout_job_t *mesh_job_alloc(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker) {
  uint32_t job_idx;
  sfu_fanout_job_t *job = sfu_pool_alloc(&mesh->job_pools[dst_worker], &job_idx);
  if (!job) {
    sfu_metric_inc("fanout_job_pool_exhausted");
    SFU_LOG_WARN("fanout mesh: job pool exhausted (worker %u -> %u)", src_worker, dst_worker);
    return NULL;
  }
  memset(job, 0, sizeof(*job));
  job->pool_index = job_idx;
  job->pool_dst = dst_worker;
  return job;
}

bool sfu_fanout_mesh_enqueue(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt, const struct sockaddr_storage *dst_addr,
                             socklen_t dst_len) {
  sfu_fanout_job_t *job = mesh_job_alloc(mesh, src_worker, dst_worker);
  if (!job) {
    return false;
  }

  SFU_LOG_DEBUG("FANOUT ENQUEUE pkt=%p src=%u dst=%u len=%u", pkt, src_worker, dst_worker, pkt->len);

  job->pkt = pkt;
  memcpy(&job->dst, dst_addr, dst_len);
  job->dst_len = dst_len;
  job->kind = SFU_FANOUT_JOB_READY;

  if (!sfu_spsc_ring_push(mesh_ring(mesh, src_worker, dst_worker), job)) {
    sfu_metric_inc("fanout_ring_full");
    SFU_LOG_WARN("fanout mesh: ring %u->%u full, dropping", src_worker, dst_worker);
    sfu_fanout_mesh_free_job(mesh, job);
    return false;
  }

  return true;
}

bool sfu_fanout_mesh_enqueue_forward(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt, sfu_peer_session_t *subscriber,
                                     sfu_peer_session_t *publisher, const struct sockaddr_storage *dst_addr, socklen_t dst_len, uint32_t video_ssrc,
                                     uint32_t video_rtx_ssrc, uint8_t video_pt, uint8_t video_rtx_pt, bool has_video, bool is_audio,
                                     const sfu_svc_descriptor_t *svc, bool has_svc, bool is_keyframe) {
  sfu_fanout_job_t *job = mesh_job_alloc(mesh, src_worker, dst_worker);
  if (!job) {
    return false;
  }

  job->pkt = pkt;
  memcpy(&job->dst, dst_addr, dst_len);
  job->dst_len = dst_len;
  job->kind = SFU_FANOUT_JOB_FORWARD;
  job->subscriber = subscriber;
  job->publisher = publisher;
  if (has_svc && svc) {
    job->svc = *svc;
  }
  job->video_ssrc = video_ssrc;
  job->video_rtx_ssrc = video_rtx_ssrc;
  job->video_pt = video_pt;
  job->video_rtx_pt = video_rtx_pt;
  job->has_video = has_video;
  job->is_audio = is_audio;
  job->has_svc = has_svc;
  job->is_keyframe = is_keyframe;

  if (!sfu_spsc_ring_push(mesh_ring(mesh, src_worker, dst_worker), job)) {
    sfu_metric_inc("fanout_ring_full");
    SFU_LOG_WARN("fanout mesh: ring %u->%u full, dropping", src_worker, dst_worker);
    sfu_fanout_mesh_free_job(mesh, job);
    return false;
  }

  return true;
}

bool sfu_fanout_mesh_enqueue_forward_batch(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *source,
                                           sfu_peer_session_t *publisher, const sfu_fanout_target_t *targets, uint8_t target_count, bool is_audio,
                                           const sfu_svc_descriptor_t *svc, bool has_svc, bool is_keyframe) {
  if (!source || !targets || target_count == 0 || target_count > SFU_FANOUT_BATCH_CAP) {
    return false;
  }
  sfu_fanout_job_t *job = mesh_job_alloc(mesh, src_worker, dst_worker);
  if (!job) {
    return false;
  }

  job->pkt = source;
  job->publisher = publisher;
  job->is_audio = is_audio;
  job->has_svc = has_svc;
  job->is_keyframe = is_keyframe;
  job->target_count = target_count;
  job->kind = SFU_FANOUT_JOB_BATCH;
  if (has_svc && svc) {
    job->svc = *svc;
  }
  if (publisher) {
    atomic_fetch_add_explicit(&publisher->refcount, 1, memory_order_relaxed);
  }
  for (uint8_t i = 0; i < target_count; i++) {
    job->targets[i] = targets[i];
    atomic_fetch_add_explicit(&targets[i].subscriber->refcount, 1, memory_order_relaxed);
  }

  if (!sfu_spsc_ring_push(mesh_ring(mesh, src_worker, dst_worker), job)) {
    sfu_metric_inc("fanout_ring_full");
    for (uint8_t i = 0; i < target_count; i++) {
      sfu_session_release(job->targets[i].subscriber);
    }
    if (publisher) {
      sfu_session_release(publisher);
    }
    sfu_fanout_mesh_free_job(mesh, job);
    return false;
  }
  sfu_metric_inc("fanout_batch_jobs");
  sfu_metric_add("fanout_batch_targets", target_count);
  return true;
}

unsigned sfu_fanout_mesh_drain(sfu_fanout_mesh_t *mesh, uint32_t dst_worker, unsigned max_count, sfu_fanout_job_fn on_job, void *user_data) {
  unsigned drained = 0;

  for (uint32_t src = 0; src < mesh->worker_count && drained < max_count; src++) {
    if (src == dst_worker) {
      continue;
    }

    sfu_spsc_ring_t *ring = mesh_ring(mesh, src, dst_worker);
    void *item;
    while (drained < max_count && sfu_spsc_ring_pop(ring, &item)) {
      on_job(user_data, (sfu_fanout_job_t *)item);
      drained++;
    }
  }

  return drained;
}

bool sfu_fanout_mesh_enqueue_keyframe_request(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_peer_session_t *publisher) {
  if (!mesh || dst_worker >= mesh->worker_count || !publisher) {
    return false;
  }

  sfu_fanout_job_t *job = mesh_job_alloc(mesh, src_worker, dst_worker);
  if (!job) {
    return false;
  }

  job->kind = SFU_FANOUT_JOB_KEYFRAME_REQUEST;
  job->publisher = publisher;

  atomic_fetch_add_explicit(&publisher->refcount, 1, memory_order_relaxed);

  if (!sfu_spsc_ring_push(mesh_ring(mesh, src_worker, dst_worker), job)) {
    sfu_metric_inc("fanout_ring_full");
    SFU_LOG_WARN("fanout mesh: ring %u->%u full, dropping", src_worker, dst_worker);
    sfu_session_release(publisher);
    sfu_fanout_mesh_free_job(mesh, job);
    return false;
  }

  return true;
}

void sfu_fanout_mesh_free_job(sfu_fanout_mesh_t *mesh, sfu_fanout_job_t *job) { sfu_pool_free(&mesh->job_pools[job->pool_dst], job->pool_index); }
