#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

#include "congestion/pacer.h"
#include "pipeline/paced_send.h"
#include "sfu/datadef.h"

static bool enqueue_packet(sfu_paced_send_t *q, const uint8_t *payload, uint16_t len, const struct sockaddr_storage *dst, uint32_t pacing_bps,
                           int64_t now_us, int64_t *release_at_us) {
  sfu_paced_send_metadata_t metadata = {0};
  sfu_pacer_reservation_t reservation = {
      .bytes = len,
      .pacer_class = SFU_PACER_CLASS_VIDEO_BASE,
      .active = true,
  };
  return sfu_paced_send_enqueue(q, payload, len, NULL, 0, dst, sizeof(struct sockaddr_in), SFU_PACER_CLASS_VIDEO_BASE, pacing_bps,
                                NULL, &reservation, &metadata, now_us, release_at_us);
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
  assert(sfu_paced_send_admit_frame_packet(&q, 10, false, false, 1000000));
  sfu_paced_send_reject_input_frame(&q);
  assert(!sfu_paced_send_admit_frame_packet(&q, 10, false, false, 1000000));
  assert(!sfu_paced_send_admit_frame_packet(&q, 10, true, false, 1000000));
  assert(sfu_paced_send_admit_frame_packet(&q, 11, true, false, 1000000));
  sfu_paced_send_destroy(&q);
}

static void test_keyframe_bypasses_delay_drop(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  q.count = 1;
  q.next_release_us = 1200000;
  assert(!sfu_paced_send_admit_frame_packet(&q, 20, true, false, 1000000));
  assert(sfu_paced_send_admit_frame_packet(&q, 21, true, true, 1000000));
  sfu_paced_send_destroy(&q);
}

int main(void) {
  test_enqueue_spacing_and_copy();
  test_size_and_rate_floor();
  test_projected_delay_includes_scan_cap();
  test_rate_change_does_not_reprice_backlog();
  test_frame_rejected_after_enqueue_failure();
  test_keyframe_bypasses_delay_drop();
  return 0;
}
