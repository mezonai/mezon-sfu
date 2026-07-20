#include "runtime/fanout.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stddef.h>
#include <string.h>

int sfu_fanout_mesh_init(sfu_fanout_mesh_t *mesh, uint32_t worker_count, uint32_t ring_capacity, uint32_t job_pool_capacity) {
  memset(mesh, 0, sizeof(*mesh));
  mesh->worker_count = worker_count;

  if (sfu_pool_init(&mesh->job_pool, job_pool_capacity, sizeof(sfu_fanout_job_t)) != 0) {
    SFU_LOG_ERROR("fanout mesh: failed to init job pool");
    return -1;
  }

  uint32_t cell_count = worker_count * worker_count;
  mesh->rings = SFU_CALLOC(cell_count, sizeof(sfu_spsc_ring_t));
  if (!mesh->rings) {
    SFU_LOG_ERROR("fanout mesh: failed to allocate ring array");
    sfu_pool_destroy(&mesh->job_pool);
    return -1;
  }

  for (uint32_t i = 0; i < cell_count; i++) {
    /* Diagonal cells (src == dst) are never used -- local fan-out
     * bypasses the mesh entirely and queues send_zc directly -- but
     * initializing them uniformly keeps indexing simple and the
     * memory cost is negligible. */
    if (sfu_spsc_ring_init(&mesh->rings[i], ring_capacity) != 0) {
      SFU_LOG_ERROR("fanout mesh: failed to init ring %u", i);
      for (uint32_t j = 0; j < i; j++) {
        sfu_spsc_ring_destroy(&mesh->rings[j]);
      }
      SFU_FREE(mesh->rings);
      sfu_pool_destroy(&mesh->job_pool);
      return -1;
    }
  }

  SFU_LOG_INFO("fanout mesh initialized: %u workers, %u rings, %u job slots", worker_count, cell_count, job_pool_capacity);
  return 0;
}

void sfu_fanout_mesh_destroy(sfu_fanout_mesh_t *mesh) {
  uint32_t cell_count = mesh->worker_count * mesh->worker_count;
  for (uint32_t i = 0; i < cell_count; i++) {
    sfu_spsc_ring_destroy(&mesh->rings[i]);
  }
  SFU_FREE(mesh->rings);
  sfu_pool_destroy(&mesh->job_pool);
}

static inline sfu_spsc_ring_t *mesh_ring(sfu_fanout_mesh_t *mesh, uint32_t src, uint32_t dst) { return &mesh->rings[src * mesh->worker_count + dst]; }

bool sfu_fanout_mesh_enqueue(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt, const struct sockaddr_storage *dst_addr,
                             socklen_t dst_len) {
  uint32_t job_idx;
  sfu_fanout_job_t *job = sfu_pool_alloc(&mesh->job_pool, &job_idx);
  if (!job) {
    SFU_LOG_WARN("fanout mesh: job pool exhausted (worker %u -> %u)", src_worker, dst_worker);
    return false;
  }

  SFU_LOG_DEBUG("FANOUT ENQUEUE pkt=%p src=%u dst=%u len=%u", pkt, src_worker, dst_worker, pkt->len);

  job->pkt = pkt;
  memcpy(&job->dst, dst_addr, dst_len);
  job->dst_len = dst_len;

  if (!sfu_spsc_ring_push(mesh_ring(mesh, src_worker, dst_worker), job)) {
    SFU_LOG_WARN("fanout mesh: ring %u->%u full, dropping", src_worker, dst_worker);
    sfu_pool_free(&mesh->job_pool, job_idx);
    return false;
  }

  return true;
}

unsigned sfu_fanout_mesh_drain(sfu_fanout_mesh_t *mesh, uint32_t dst_worker, unsigned max_count, sfu_fanout_job_fn on_job, void *user_data) {
  unsigned drained = 0;

  for (uint32_t src = 0; src < mesh->worker_count && drained < max_count; src++) {
    if (src == dst_worker) {
      continue; /* diagonal unused */
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

void sfu_fanout_mesh_free_job(sfu_fanout_mesh_t *mesh, sfu_fanout_job_t *job) {
  /* Jobs are allocated from a plain sfu_pool_t of fixed-size slots;
   * recover the slot index from the pointer offset rather than
   * threading an index through the whole call chain. */
  ptrdiff_t byte_off = (uint8_t *)job - mesh->job_pool.slab;
  uint32_t index = (uint32_t)(byte_off / mesh->job_pool.slot_size);
  sfu_pool_free(&mesh->job_pool, index);
}
