#ifndef SFU_CONGESTION_BANDWIDTH_ALLOCATOR_H
#define SFU_CONGESTION_BANDWIDTH_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS 16u
#define SFU_BANDWIDTH_ALLOCATOR_MAX_PUBLISHERS SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS
#define SFU_BANDWIDTH_VIDEO_POOL_PERCENT 85u
#define SFU_BANDWIDTH_STREAM_CAP_BPS 1500000u

typedef enum sfu_bandwidth_stream_kind {
  SFU_BANDWIDTH_STREAM_CAMERA = 1,
  SFU_BANDWIDTH_STREAM_SCREEN = 2,
} sfu_bandwidth_stream_kind_t;

typedef struct sfu_bandwidth_stream_input {
  uint32_t publisher_peer_id;
  uint32_t remote_slot;
  uint64_t assignment_generation;
  sfu_bandwidth_stream_kind_t kind;
  bool active;
} sfu_bandwidth_stream_input_t;

typedef struct sfu_bandwidth_stream_allocation {
  uint32_t publisher_peer_id;
  uint32_t remote_slot;
  uint64_t assignment_generation;
  sfu_bandwidth_stream_kind_t kind;
  uint32_t allocated_bps;
} sfu_bandwidth_stream_allocation_t;

typedef struct sfu_bandwidth_publisher_allocation {
  uint32_t publisher_peer_id;
  uint32_t allocated_bps;
} sfu_bandwidth_publisher_allocation_t;

typedef struct sfu_bandwidth_allocation {
  uint64_t estimated_bps;
  uint64_t reserve_bps;
  uint64_t video_pool_bps;
  uint64_t allocated_bps;
  uint64_t unallocated_bps;
  size_t stream_count;
  size_t publisher_count;
  sfu_bandwidth_stream_allocation_t streams[SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS];
  sfu_bandwidth_publisher_allocation_t publishers[SFU_BANDWIDTH_ALLOCATOR_MAX_PUBLISHERS];
} sfu_bandwidth_allocation_t;

void sfu_bandwidth_allocate(const sfu_bandwidth_stream_input_t *inputs, size_t input_count, uint64_t estimated_bps,
                            sfu_bandwidth_allocation_t *allocation);

#endif /* SFU_CONGESTION_BANDWIDTH_ALLOCATOR_H */
