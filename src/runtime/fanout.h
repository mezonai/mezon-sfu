#ifndef SFU_RUNTIME_FANOUT_H
#define SFU_RUNTIME_FANOUT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "media/svc/svc_descriptor.h"
#include "memory/pool.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"
#include "util/ringbuffer.h"

#define SFU_FANOUT_BATCH_CAP 16

typedef struct sfu_fanout_target {
  struct sockaddr_storage dst;
  socklen_t dst_len;
  sfu_peer_session_t *subscriber;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint64_t assignment_generation;
  uint32_t remote_slot;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  uint8_t source;
  bool has_video;
} sfu_fanout_target_t;

typedef enum sfu_fanout_job_kind {
  SFU_FANOUT_JOB_READY = 0,
  SFU_FANOUT_JOB_FORWARD = 1,
  SFU_FANOUT_JOB_KEYFRAME_REQUEST = 2,
  SFU_FANOUT_JOB_BATCH = 3,
  SFU_FANOUT_JOB_INGRESS = 4,
} sfu_fanout_job_kind_t;

typedef struct sfu_fanout_job {
  sfu_packet_t *pkt;
  struct sockaddr_storage dst;
  socklen_t dst_len;
  sfu_peer_session_t *subscriber;
  sfu_peer_session_t *publisher;
  sfu_svc_descriptor_t svc;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint64_t assignment_generation;
  uint32_t remote_slot;
  uint32_t pool_index;
  uint32_t pool_dst;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  uint8_t source;
  bool has_video;
  bool is_audio;
  bool has_svc;
  bool is_keyframe;
  uint8_t target_count;
  sfu_fanout_target_t targets[SFU_FANOUT_BATCH_CAP];
  uint8_t kind;
} sfu_fanout_job_t;

typedef struct sfu_fanout_mesh {
  sfu_pool_t *job_pools;
  sfu_spsc_ring_t *rings;
  sfu_spsc_ring_t *return_rings;
  uint32_t worker_count;
  uint32_t per_partition_capacity;
  /* Rotating cursors so drain scans start at a different source partition on
   * each call, preventing the lowest-indexed source (or a busy low-indexed
   * source) from starving the others when max_count caps per-call work. */
  _Atomic uint32_t drain_cursor;
  _Atomic uint32_t return_cursor;
} sfu_fanout_mesh_t;

typedef void (*sfu_fanout_job_fn)(void *user_data, sfu_fanout_job_t *job);
typedef void (*sfu_fanout_return_fn)(void *user_data, uintptr_t token);

int sfu_fanout_mesh_init(sfu_fanout_mesh_t *mesh, uint32_t worker_count, uint32_t ring_capacity, uint32_t job_pool_capacity);
void sfu_fanout_mesh_destroy(sfu_fanout_mesh_t *mesh);

bool sfu_fanout_mesh_enqueue(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt, const struct sockaddr_storage *dst_addr,
                             socklen_t dst_len);
bool sfu_fanout_mesh_enqueue_forward_batch(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *source,
                                           sfu_peer_session_t *publisher, const sfu_fanout_target_t *targets, uint8_t target_count, bool is_audio,
                                           const sfu_svc_descriptor_t *svc, bool has_svc, bool is_keyframe);
bool sfu_fanout_mesh_enqueue_keyframe_request_for_source(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker,
                                                         sfu_peer_session_t *publisher, sfu_media_kind_t source);
bool sfu_fanout_mesh_enqueue_keyframe_request(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_peer_session_t *publisher);
bool sfu_fanout_mesh_enqueue_ingress(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt);
bool sfu_fanout_mesh_return_rx_frame(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, uintptr_t token);
unsigned sfu_fanout_mesh_drain(sfu_fanout_mesh_t *mesh, uint32_t dst_worker, unsigned max_count, sfu_fanout_job_fn on_job, void *user_data);
unsigned sfu_fanout_mesh_drain_returns(sfu_fanout_mesh_t *mesh, uint32_t dst_worker, unsigned max_count, sfu_fanout_return_fn on_return, void *user_data);
void sfu_fanout_mesh_free_job(sfu_fanout_mesh_t *mesh, sfu_fanout_job_t *job);

#endif /* SFU_RUNTIME_FANOUT_H */
