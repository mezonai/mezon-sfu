#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "congestion/bandwidth_allocator.h"

static uint64_t estimate_for_pool(uint64_t pool) {
  uint64_t estimate = pool * 100u / SFU_BANDWIDTH_VIDEO_POOL_PERCENT;
  while (estimate / 100u * SFU_BANDWIDTH_VIDEO_POOL_PERCENT +
             estimate % 100u * SFU_BANDWIDTH_VIDEO_POOL_PERCENT / 100u <
         pool) {
    estimate++;
  }
  return estimate;
}

static const sfu_bandwidth_stream_allocation_t *find_stream(const sfu_bandwidth_allocation_t *a, uint32_t publisher,
                                                             sfu_bandwidth_stream_kind_t kind) {
  for (size_t i = 0; i < a->stream_count; i++) {
    if (a->streams[i].publisher_peer_id == publisher && a->streams[i].kind == kind) return &a->streams[i];
  }
  return NULL;
}

static uint32_t publisher_total(const sfu_bandwidth_allocation_t *a, uint32_t publisher) {
  for (size_t i = 0; i < a->publisher_count; i++) {
    if (a->publishers[i].publisher_peer_id == publisher) return a->publishers[i].allocated_bps;
  }
  return 0;
}

static sfu_bandwidth_stream_input_t input(uint32_t publisher, uint32_t slot, uint64_t generation,
                                          sfu_bandwidth_stream_kind_t kind) {
  return (sfu_bandwidth_stream_input_t){
      .publisher_peer_id = publisher,
      .remote_slot = slot,
      .assignment_generation = generation,
      .kind = kind,
      .active = true,
  };
}

static void test_empty_and_inactive(void) {
  sfu_bandwidth_allocation_t a;
  sfu_bandwidth_allocate(NULL, 0, 1000000, &a);
  assert(a.stream_count == 0 && a.allocated_bps == 0);
  assert(a.video_pool_bps == 850000 && a.reserve_bps == 150000 && a.unallocated_bps == 850000);

  sfu_bandwidth_stream_input_t inactive = input(1, 0, 1, SFU_BANDWIDTH_STREAM_CAMERA);
  inactive.active = false;
  sfu_bandwidth_allocate(&inactive, 1, 1000000, &a);
  assert(a.stream_count == 0 && a.publisher_count == 0);
}

static void test_threshold_boundaries_and_screen_priority(void) {
  sfu_bandwidth_stream_input_t streams[] = {
      input(10, 0, 1, SFU_BANDWIDTH_STREAM_CAMERA),
      input(20, 1, 1, SFU_BANDWIDTH_STREAM_SCREEN),
  };
  sfu_bandwidth_allocation_t a;

  sfu_bandwidth_allocate(streams, 2, estimate_for_pool(240000), &a);
  assert(find_stream(&a, 20, SFU_BANDWIDTH_STREAM_SCREEN)->allocated_bps == 240000);
  assert(find_stream(&a, 10, SFU_BANDWIDTH_STREAM_CAMERA)->allocated_bps == 0);

  sfu_bandwidth_allocate(streams, 2, estimate_for_pool(480000), &a);
  assert(find_stream(&a, 20, SFU_BANDWIDTH_STREAM_SCREEN)->allocated_bps == 240000);
  assert(find_stream(&a, 10, SFU_BANDWIDTH_STREAM_CAMERA)->allocated_bps == 240000);

  sfu_bandwidth_allocate(streams, 2, estimate_for_pool(960000), &a);
  assert(find_stream(&a, 20, SFU_BANDWIDTH_STREAM_SCREEN)->allocated_bps == 720000);
  assert(find_stream(&a, 10, SFU_BANDWIDTH_STREAM_CAMERA)->allocated_bps == 240000);

  sfu_bandwidth_allocate(streams, 2, estimate_for_pool(1440000), &a);
  assert(find_stream(&a, 20, SFU_BANDWIDTH_STREAM_SCREEN)->allocated_bps == 720000);
  assert(find_stream(&a, 10, SFU_BANDWIDTH_STREAM_CAMERA)->allocated_bps == 720000);

  sfu_bandwidth_allocate(streams, 2, estimate_for_pool(2160000), &a);
  assert(find_stream(&a, 20, SFU_BANDWIDTH_STREAM_SCREEN)->allocated_bps == 1440000);
  assert(find_stream(&a, 10, SFU_BANDWIDTH_STREAM_CAMERA)->allocated_bps == 720000);

  sfu_bandwidth_allocate(streams, 2, estimate_for_pool(2880000), &a);
  assert(find_stream(&a, 20, SFU_BANDWIDTH_STREAM_SCREEN)->allocated_bps == 1440000);
  assert(find_stream(&a, 10, SFU_BANDWIDTH_STREAM_CAMERA)->allocated_bps == 1440000);
}

static void test_equal_sharing_remainder_and_stable_order(void) {
  sfu_bandwidth_stream_input_t streams[] = {
      input(30, 2, 7, SFU_BANDWIDTH_STREAM_CAMERA),
      input(10, 0, 5, SFU_BANDWIDTH_STREAM_CAMERA),
      input(20, 1, 6, SFU_BANDWIDTH_STREAM_CAMERA),
  };
  sfu_bandwidth_allocation_t a;
  sfu_bandwidth_allocate(streams, 3, estimate_for_pool(10), &a);
  assert(a.stream_count == 3 && a.allocated_bps == 10);
  assert(a.streams[0].publisher_peer_id == 10 && a.streams[0].allocated_bps == 4);
  assert(a.streams[1].publisher_peer_id == 20 && a.streams[1].allocated_bps == 3);
  assert(a.streams[2].publisher_peer_id == 30 && a.streams[2].allocated_bps == 3);

  sfu_bandwidth_stream_input_t permuted[] = {streams[1], streams[2], streams[0]};
  sfu_bandwidth_allocation_t b;
  sfu_bandwidth_allocate(permuted, 3, estimate_for_pool(10), &b);
  assert(memcmp(a.streams, b.streams, sizeof(a.streams)) == 0);
}

static void test_publisher_aggregation_duplicates_and_generation(void) {
  sfu_bandwidth_stream_input_t streams[] = {
      input(7, 3, 11, SFU_BANDWIDTH_STREAM_CAMERA),
      input(7, 3, 11, SFU_BANDWIDTH_STREAM_CAMERA),
      input(7, 3, 11, SFU_BANDWIDTH_STREAM_SCREEN),
      input(8, 4, 12, SFU_BANDWIDTH_STREAM_CAMERA),
  };
  sfu_bandwidth_allocation_t a;
  sfu_bandwidth_allocate(streams, 4, estimate_for_pool(720000), &a);
  assert(a.stream_count == 3);
  assert(a.publisher_count == 2);
  uint32_t publisher7 = find_stream(&a, 7, SFU_BANDWIDTH_STREAM_CAMERA)->allocated_bps +
                        find_stream(&a, 7, SFU_BANDWIDTH_STREAM_SCREEN)->allocated_bps;
  assert(publisher_total(&a, 7) == publisher7);
  assert((uint64_t)publisher_total(&a, 7) + publisher_total(&a, 8) == a.allocated_bps);

  streams[1].assignment_generation = 12;
  sfu_bandwidth_allocate(streams, 4, estimate_for_pool(720000), &a);
  assert(a.stream_count == 4);
}

static void test_caps_safe_pool_and_overflow(void) {
  sfu_bandwidth_stream_input_t streams[] = {
      input(1, 0, 1, SFU_BANDWIDTH_STREAM_CAMERA),
      input(1, 0, 1, SFU_BANDWIDTH_STREAM_SCREEN),
      input(2, 1, 2, SFU_BANDWIDTH_STREAM_CAMERA),
  };
  sfu_bandwidth_allocation_t a;
  sfu_bandwidth_allocate(streams, 3, UINT64_MAX, &a);
  assert(a.video_pool_bps <= UINT64_MAX - a.reserve_bps);
  assert(a.allocated_bps <= a.video_pool_bps);
  assert(a.allocated_bps == 3u * SFU_BANDWIDTH_STREAM_CAP_BPS);
  assert(a.unallocated_bps == a.video_pool_bps - a.allocated_bps);
  for (size_t i = 0; i < a.stream_count; i++) assert(a.streams[i].allocated_bps <= SFU_BANDWIDTH_STREAM_CAP_BPS);

  sfu_bandwidth_allocate(streams, 3, UINT32_MAX, &a);
  assert(a.allocated_bps <= a.video_pool_bps);
  uint64_t publisher_sum = 0;
  for (size_t i = 0; i < a.publisher_count; i++) publisher_sum += a.publishers[i].allocated_bps;
  assert(publisher_sum == a.allocated_bps);
}

int main(void) {
  test_empty_and_inactive();
  test_threshold_boundaries_and_screen_priority();
  test_equal_sharing_remainder_and_stable_order();
  test_publisher_aggregation_duplicates_and_generation();
  test_caps_safe_pool_and_overflow();
  printf("test_bandwidth_allocator: OK\n");
  return 0;
}
