#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

#include "congestion/pacer.h"
#include "pipeline/paced_send.h"
#include "sfu/datadef.h"

static bool enqueue_packet_full(sfu_paced_send_t *q, const uint8_t *payload, uint16_t len, const struct sockaddr_storage *dst, uint32_t pacing_bps,
                                int64_t now_us, int64_t *release_at_us, uint32_t publisher_peer_id, bool is_keyframe) {
  sfu_paced_send_metadata_t metadata = {0};
  metadata.publisher_peer_id = publisher_peer_id;
  metadata.is_keyframe = is_keyframe;
  sfu_pacer_reservation_t reservation = {
      .bytes = len,
      .pacer_class = SFU_PACER_CLASS_VIDEO_BASE,
      .active = true,
  };
  bool enqueued = sfu_paced_send_enqueue(q, payload, len, NULL, 0, dst, sizeof(struct sockaddr_in), SFU_PACER_CLASS_VIDEO_BASE, pacing_bps,
                                         NULL, &reservation, &metadata, now_us, release_at_us);
  if (enqueued) {
    q->ready_count++;
  }
  return enqueued;
}

static bool enqueue_packet_for_publisher(sfu_paced_send_t *q, const uint8_t *payload, uint16_t len, const struct sockaddr_storage *dst, uint32_t pacing_bps,
                                        int64_t now_us, int64_t *release_at_us, uint32_t publisher_peer_id) {
  return enqueue_packet_full(q, payload, len, dst, pacing_bps, now_us, release_at_us, publisher_peer_id, false);
}

static bool enqueue_packet(sfu_paced_send_t *q, const uint8_t *payload, uint16_t len, const struct sockaddr_storage *dst, uint32_t pacing_bps,
                           int64_t now_us, int64_t *release_at_us) {
  return enqueue_packet_for_publisher(q, payload, len, dst, pacing_bps, now_us, release_at_us, 0);
}

static void test_enqueue_spacing_and_copy(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1000];
  memset(payload, 0x5a, sizeof(payload));
  struct sockaddr_storage dst = {0};
  int64_t first = -1, second = -1;
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, 1000000, 1000000, &first));
  payload[0] = 0;
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, 1000000, 1000000, &second));
  assert(q.count == 2);
  assert(first == 1000000);
  assert(second == 1004000);
  assert(q.entries[q.head].data[0] == 0x5a);
  sfu_paced_send_destroy(&q);
}

static void test_size_and_rate_floor(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[SFU_PACED_SEND_MAX_PAYLOAD + 1] = {0};
  struct sockaddr_storage dst = {0};
  int64_t first, second;
  assert(!enqueue_packet(&q, payload, sizeof(payload), &dst, 1, 0, &first));
  assert(enqueue_packet(&q, payload, 1000, &dst, 1, 2000000, &first));
  assert(enqueue_packet(&q, payload, 1000, &dst, 1, 2000000, &second));
  assert(first == 2000000);
  assert(second == 2004000);
  sfu_paced_send_destroy(&q);
}

static void test_projected_delay_includes_scan_cap(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1] = {0};
  struct sockaddr_storage dst = {0};
  for (int i = 0; i < 9; i++) assert(enqueue_packet(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL));
  assert(sfu_paced_send_projected_delay_us(&q, 1000000) >= 3 * SFU_PACED_SEND_SCAN_INTERVAL_US);
  sfu_paced_send_destroy(&q);
}

static void test_rate_change_does_not_reprice_backlog(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1000] = {0};
  struct sockaddr_storage dst = {0};
  int64_t first, second, third;
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, 2000000, 1000000, &first));
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, 2000000, 1000000, &second));
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, 8000000, 1000000, &third));
  assert(first == 1000000);
  assert(second == 1004000);
  assert(third == 1008000);
  sfu_paced_send_destroy(&q);
}

static void test_frame_rejected_after_enqueue_failure(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  assert(sfu_paced_send_admit_frame_packet(&q, 10, false, false, true, SFU_PACED_SEND_CAMERA_MAX_DELAY_US, 1000000));
  sfu_paced_send_reject_input_frame(&q);
  assert(!sfu_paced_send_admit_frame_packet(&q, 10, false, false, true, SFU_PACED_SEND_CAMERA_MAX_DELAY_US, 1000000));
  assert(!sfu_paced_send_admit_frame_packet(&q, 10, true, false, true, SFU_PACED_SEND_CAMERA_MAX_DELAY_US, 1000000));
  assert(sfu_paced_send_admit_frame_packet(&q, 11, true, false, true, SFU_PACED_SEND_CAMERA_MAX_DELAY_US, 1000000));
  sfu_paced_send_destroy(&q);
}

static void test_keyframe_bypasses_delay_drop(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  q.count = 1;
  q.next_release_us = 1200000;
  assert(!sfu_paced_send_admit_frame_packet(&q, 20, true, false, true, SFU_PACED_SEND_CAMERA_MAX_DELAY_US, 1000000));
  assert(sfu_paced_send_admit_frame_packet(&q, 21, true, true, true, SFU_PACED_SEND_CAMERA_MAX_DELAY_US, 1000000));
  sfu_paced_send_destroy(&q);
}

static void test_screen_backlog_evicts_whole_frames(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1] = {0};
  struct sockaddr_storage dst = {0};
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL));
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL));
  q.entries[1].metadata.frame_end = true;
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL));
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL));
  q.entries[3].metadata.frame_end = true;
  q.entries[0].span_us = 200000;
  q.entries[1].span_us = 200000;
  q.entries[2].span_us = 200000;
  q.entries[3].span_us = 200000;
  q.entries[0].release_at_us = 1000000;
  q.entries[1].release_at_us = 1200000;
  q.entries[2].release_at_us = 1400000;
  q.entries[3].release_at_us = 1600000;
  q.next_release_us = 1800000;

  sfu_paced_send_drop_report_t report;
  sfu_paced_send_drop_report_init(&report);
  assert(sfu_paced_send_bound_backlog(&q, SFU_PACED_SEND_SCREEN_MAX_DELAY_US, 1000000, &report));
  assert(report.frames == 1);
  assert(q.count == 2);
  assert(q.ready_count == 2);
  assert(q.head == 2);
  assert(q.entries[q.head].release_at_us == 1000000);
  assert(q.next_release_us == 1400000);
  assert(q.dropped_delay_frames == 1);
  sfu_paced_send_destroy(&q);
}

/* A screen queue is per subscriber, so one trim can discard frames belonging to
 * several publishers. The report must name each one exactly once so egress can
 * invalidate the matching layer schedulers and leave the others decoding. */
static void test_backlog_report_attributes_dropped_publishers(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1] = {0};
  struct sockaddr_storage dst = {0};

  /* Frame 1: publisher 7, two packets. */
  assert(enqueue_packet_for_publisher(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 7));
  assert(enqueue_packet_for_publisher(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 7));
  q.entries[1].metadata.frame_end = true;
  /* Frame 2: publisher 9, two packets. */
  assert(enqueue_packet_for_publisher(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 9));
  assert(enqueue_packet_for_publisher(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 9));
  q.entries[3].metadata.frame_end = true;

  for (uint32_t i = 0; i < 4; i++) {
    q.entries[i].span_us = 200000;
    q.entries[i].release_at_us = 1000000 + (int64_t)i * 200000;
  }
  q.next_release_us = 1800000;

  sfu_paced_send_drop_report_t report;
  sfu_paced_send_drop_report_init(&report);
  assert(sfu_paced_send_bound_backlog(&q, SFU_PACED_SEND_SCREEN_MAX_DELAY_US, 1000000, &report));
  assert(!report.truncated);
  assert(report.frames == 1);
  assert(report.publisher_count == 1);
  assert(report.publisher_peer_ids[0] == 7);

  /* A second trim now takes publisher 9's frame. */
  assert(sfu_paced_send_bound_backlog(&q, 250000, 1000000, &report));
  assert(report.frames == 2);
  assert(report.publisher_count == 2);
  assert(report.publisher_peer_ids[1] == 9);
  sfu_paced_send_destroy(&q);
}

/* A frame whose publisher identity is unknown (0) must still be counted, or
 * drops would go unreported and egress would never request a keyframe. */
static void test_backlog_report_counts_frames_with_unknown_publisher(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1] = {0};
  struct sockaddr_storage dst = {0};
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL));
  assert(enqueue_packet(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL));
  q.entries[1].metadata.frame_end = true;
  q.entries[0].span_us = 200000;
  q.entries[1].span_us = 200000;
  q.entries[0].release_at_us = 1000000;
  q.entries[1].release_at_us = 1200000;
  q.next_release_us = 1400000;

  sfu_paced_send_drop_report_t report;
  sfu_paced_send_drop_report_init(&report);
  /* Projected delay is 400000us, under the screen limit but over this one. */
  assert(sfu_paced_send_bound_backlog(&q, 250000, 1000000, &report));
  assert(report.frames == 1);
  assert(report.publisher_count == 0);
  sfu_paced_send_destroy(&q);
}

static void test_keyframe_pacing_rate_floor(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1000] = {0};
  struct sockaddr_storage dst = {0};
  int64_t first = -1, second = -1;
  assert(enqueue_packet_full(&q, payload, 1000, &dst, 1, 2000000, &first, 0, true));
  assert(enqueue_packet_full(&q, payload, 1000, &dst, 1, 2000000, &second, 0, true));
  assert(first == 2000000);
  /* Floor for keyframe is 5 Mbps: span is (1000 * 8 * 1000000 + 5000000 - 1) / 5000000 = 1600 us */
  assert(second == 2001600);
  sfu_paced_send_destroy(&q);
}

static void test_backlog_preserves_keyframe(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1] = {0};
  struct sockaddr_storage dst = {0};
  assert(enqueue_packet_full(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 5, true));
  assert(enqueue_packet_full(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 5, true));
  q.entries[1].metadata.frame_end = true;
  q.entries[0].span_us = 450000;
  q.entries[1].span_us = 450000;
  q.entries[0].release_at_us = 1000000;
  q.entries[1].release_at_us = 1450000;
  q.next_release_us = 1900000; /* 900ms delay, over SFU_PACED_SEND_SCREEN_MAX_DELAY_US (750ms) */

  sfu_paced_send_drop_report_t report;
  sfu_paced_send_drop_report_init(&report);
  /* bound_backlog must NOT drop the keyframe */
  bool under_delay = sfu_paced_send_bound_backlog(&q, SFU_PACED_SEND_SCREEN_MAX_DELAY_US, 1000000, &report);
  assert(!under_delay);
  assert(report.frames == 0);
  assert(q.count == 2);
  assert(q.head == 0);
  sfu_paced_send_destroy(&q);
}

static void test_backlog_drops_delta_before_keyframe(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  uint8_t payload[1] = {0};
  struct sockaddr_storage dst = {0};
  /* Frame 1: delta frame (not keyframe) */
  assert(enqueue_packet_full(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 3, false));
  assert(enqueue_packet_full(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 3, false));
  q.entries[1].metadata.frame_end = true;
  /* Frame 2: keyframe */
  assert(enqueue_packet_full(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 3, true));
  assert(enqueue_packet_full(&q, payload, sizeof(payload), &dst, UINT32_MAX, 1000000, NULL, 3, true));
  q.entries[3].metadata.frame_end = true;

  for (uint32_t i = 0; i < 4; i++) {
    q.entries[i].span_us = 200000;
    q.entries[i].release_at_us = 1000000 + (int64_t)i * 200000;
  }
  q.next_release_us = 1800000; /* 800ms delay */

  sfu_paced_send_drop_report_t report;
  sfu_paced_send_drop_report_init(&report);
  /* bound_backlog drops Frame 1 (delta), stops at Frame 2 (keyframe) */
  assert(sfu_paced_send_bound_backlog(&q, SFU_PACED_SEND_SCREEN_MAX_DELAY_US, 1000000, &report));
  assert(report.frames == 1);
  assert(report.publisher_peer_ids[0] == 3);
  assert(q.count == 2);
  assert(q.head == 2);
  assert(q.entries[q.head].metadata.is_keyframe);
  sfu_paced_send_destroy(&q);
}

int main(void) {
  test_enqueue_spacing_and_copy();
  test_size_and_rate_floor();
  test_keyframe_pacing_rate_floor();
  test_projected_delay_includes_scan_cap();
  test_rate_change_does_not_reprice_backlog();
  test_frame_rejected_after_enqueue_failure();
  test_keyframe_bypasses_delay_drop();
  test_screen_backlog_evicts_whole_frames();
  test_backlog_preserves_keyframe();
  test_backlog_drops_delta_before_keyframe();
  test_backlog_report_attributes_dropped_publishers();
  test_backlog_report_counts_frames_with_unknown_publisher();
  return 0;
}
