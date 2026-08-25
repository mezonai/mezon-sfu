#include "congestion/bandwidth_allocator.h"

#include "util/metrics.h"

#include <limits.h>
#include <string.h>

static bool stream_key_less(const sfu_bandwidth_stream_input_t *a, const sfu_bandwidth_stream_input_t *b) {
  if (a->remote_slot != b->remote_slot) return a->remote_slot < b->remote_slot;
  if (a->publisher_peer_id != b->publisher_peer_id) return a->publisher_peer_id < b->publisher_peer_id;
  if (a->kind != b->kind) return a->kind < b->kind;
  return a->assignment_generation < b->assignment_generation;
}

static bool same_stream_key(const sfu_bandwidth_stream_input_t *a, const sfu_bandwidth_stream_input_t *b) {
  return a->publisher_peer_id == b->publisher_peer_id && a->remote_slot == b->remote_slot &&
         a->assignment_generation == b->assignment_generation && a->kind == b->kind;
}

static void fill_class(sfu_bandwidth_allocation_t *allocation, sfu_bandwidth_stream_kind_t kind, uint32_t target_bps,
                       uint64_t *remaining_bps) {
  size_t eligible[SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS];
  size_t eligible_count = 0;
  uint64_t need = 0;
  for (size_t i = 0; i < allocation->stream_count; i++) {
    if (allocation->streams[i].kind == kind && allocation->streams[i].allocated_bps < target_bps) {
      eligible[eligible_count++] = i;
      need += target_bps - allocation->streams[i].allocated_bps;
    }
  }
  if (eligible_count == 0 || *remaining_bps == 0) return;
  if (*remaining_bps >= need) {
    for (size_t i = 0; i < eligible_count; i++) allocation->streams[eligible[i]].allocated_bps = target_bps;
    *remaining_bps -= need;
    return;
  }

  uint64_t share = *remaining_bps / eligible_count;
  uint64_t remainder = *remaining_bps % eligible_count;
  for (size_t i = 0; i < eligible_count; i++) {
    uint64_t increment = share + (i < remainder ? 1u : 0u);
    uint64_t deficit = target_bps - allocation->streams[eligible[i]].allocated_bps;
    if (increment > deficit) increment = deficit;
    allocation->streams[eligible[i]].allocated_bps += (uint32_t)increment;
    *remaining_bps -= increment;
  }
}

static void record_allocation_metrics(const sfu_bandwidth_allocation_t *allocation) {
  sfu_metric_inc("bandwidth_allocator_runs");
  sfu_metric_add("bandwidth_allocator_active_streams", allocation->stream_count);
  sfu_metric_add("bandwidth_allocator_unallocated_bps", allocation->unallocated_bps);
}

void sfu_bandwidth_allocate(const sfu_bandwidth_stream_input_t *inputs, size_t input_count, uint64_t estimated_bps,
                            sfu_bandwidth_allocation_t *allocation) {
  if (!allocation) return;
  memset(allocation, 0, sizeof(*allocation));
  allocation->estimated_bps = estimated_bps;
  allocation->video_pool_bps = estimated_bps / 100u * SFU_BANDWIDTH_VIDEO_POOL_PERCENT +
                               estimated_bps % 100u * SFU_BANDWIDTH_VIDEO_POOL_PERCENT / 100u;
  allocation->reserve_bps = estimated_bps - allocation->video_pool_bps;
  allocation->unallocated_bps = allocation->video_pool_bps;
  if (!inputs || input_count == 0 || allocation->video_pool_bps == 0) {
    record_allocation_metrics(allocation);
    return;
  }

  size_t order[SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS];
  size_t order_count = 0;
  for (size_t i = 0; i < input_count; i++) {
    if (!inputs[i].active || inputs[i].publisher_peer_id == 0 ||
        (inputs[i].kind != SFU_BANDWIDTH_STREAM_CAMERA && inputs[i].kind != SFU_BANDWIDTH_STREAM_SCREEN)) {
      continue;
    }
    bool duplicate = false;
    for (size_t j = 0; j < order_count; j++) {
      if (same_stream_key(&inputs[i], &inputs[order[j]])) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    if (order_count == SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS && !stream_key_less(&inputs[i], &inputs[order[order_count - 1]])) {
      continue;
    }
    size_t pos = order_count < SFU_BANDWIDTH_ALLOCATOR_MAX_STREAMS ? order_count++ : order_count - 1;
    while (pos > 0 && stream_key_less(&inputs[i], &inputs[order[pos - 1]])) {
      order[pos] = order[pos - 1];
      pos--;
    }
    order[pos] = i;
  }

  for (size_t i = 0; i < order_count; i++) {
    const sfu_bandwidth_stream_input_t *input = &inputs[order[i]];
    if (i > 0 && same_stream_key(input, &inputs[order[i - 1]])) continue;
    sfu_bandwidth_stream_allocation_t *stream = &allocation->streams[allocation->stream_count++];
    stream->publisher_peer_id = input->publisher_peer_id;
    stream->remote_slot = input->remote_slot;
    stream->assignment_generation = input->assignment_generation;
    stream->kind = input->kind;
  }

  uint64_t remaining = allocation->video_pool_bps;
  static const uint32_t thresholds[] = {SFU_BANDWIDTH_SOURCE_ADMISSION_BPS, 720000u, 1440000u};
  for (size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]) && remaining > 0; i++) {
    uint32_t screen_target = thresholds[i] < SFU_BANDWIDTH_SCREEN_CAP_BPS ? thresholds[i] : SFU_BANDWIDTH_SCREEN_CAP_BPS;
    uint32_t camera_target = thresholds[i] < SFU_BANDWIDTH_CAMERA_CAP_BPS ? thresholds[i] : SFU_BANDWIDTH_CAMERA_CAP_BPS;
    fill_class(allocation, SFU_BANDWIDTH_STREAM_SCREEN, screen_target, &remaining);
    fill_class(allocation, SFU_BANDWIDTH_STREAM_CAMERA, camera_target, &remaining);
  }
  fill_class(allocation, SFU_BANDWIDTH_STREAM_SCREEN, SFU_BANDWIDTH_SCREEN_CAP_BPS, &remaining);
  fill_class(allocation, SFU_BANDWIDTH_STREAM_CAMERA, SFU_BANDWIDTH_CAMERA_CAP_BPS, &remaining);

  for (size_t i = 0; i < allocation->stream_count; i++) {
    const sfu_bandwidth_stream_allocation_t *stream = &allocation->streams[i];
    allocation->allocated_bps += stream->allocated_bps;
    size_t publisher_index = 0;
    while (publisher_index < allocation->publisher_count &&
           allocation->publishers[publisher_index].publisher_peer_id != stream->publisher_peer_id) {
      publisher_index++;
    }
    if (publisher_index == allocation->publisher_count) {
      allocation->publishers[publisher_index].publisher_peer_id = stream->publisher_peer_id;
      allocation->publisher_count++;
    }
    uint64_t publisher_total = (uint64_t)allocation->publishers[publisher_index].allocated_bps + stream->allocated_bps;
    allocation->publishers[publisher_index].allocated_bps = publisher_total > UINT32_MAX ? UINT32_MAX : (uint32_t)publisher_total;
  }
  allocation->unallocated_bps = allocation->video_pool_bps - allocation->allocated_bps;
  record_allocation_metrics(allocation);
}
