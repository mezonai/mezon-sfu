#ifndef SFU_RUNTIME_FANOUT_H
#define SFU_RUNTIME_FANOUT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "media/svc/svc_descriptor.h"
#include "memory/pool.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"
#include "util/ringbuffer.h"

typedef enum sfu_fanout_job_kind {
  SFU_FANOUT_JOB_READY = 0,
  SFU_FANOUT_JOB_FORWARD = 1,
  SFU_FANOUT_JOB_KEYFRAME_REQUEST = 2,
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
  uint32_t pool_index;
  uint32_t pool_dst;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  bool has_video;
  bool is_audio;
  bool has_svc;
  bool is_keyframe;
  uint8_t kind;
} sfu_fanout_job_t;

typedef struct sfu_fanout_mesh {
  sfu_pool_t *job_pools;
  sfu_spsc_ring_t *rings;
  uint32_t worker_count;
  uint32_t per_partition_capacity;
} sfu_fanout_mesh_t;

typedef void (*sfu_fanout_job_fn)(void *user_data, sfu_fanout_job_t *job);

int sfu_fanout_mesh_init(sfu_fanout_mesh_t *mesh, uint32_t worker_count, uint32_t ring_capacity, uint32_t job_pool_capacity);
void sfu_fanout_mesh_destroy(sfu_fanout_mesh_t *mesh);

bool sfu_fanout_mesh_enqueue(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt, const struct sockaddr_storage *dst_addr,
                             socklen_t dst_len);
bool sfu_fanout_mesh_enqueue_forward(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_packet_t *pkt, sfu_peer_session_t *subscriber,
                                     sfu_peer_session_t *publisher, const struct sockaddr_storage *dst_addr, socklen_t dst_len, uint32_t video_ssrc,
                                     uint32_t video_rtx_ssrc, uint8_t video_pt, uint8_t video_rtx_pt, bool has_video, bool is_audio,
                                     const sfu_svc_descriptor_t *svc, bool has_svc, bool is_keyframe);
bool sfu_fanout_mesh_enqueue_keyframe_request(sfu_fanout_mesh_t *mesh, uint32_t src_worker, uint32_t dst_worker, sfu_peer_session_t *publisher);
unsigned sfu_fanout_mesh_drain(sfu_fanout_mesh_t *mesh, uint32_t dst_worker, unsigned max_count, sfu_fanout_job_fn on_job, void *user_data);
void sfu_fanout_mesh_free_job(sfu_fanout_mesh_t *mesh, sfu_fanout_job_t *job);

#endif /* SFU_RUNTIME_FANOUT_H */
