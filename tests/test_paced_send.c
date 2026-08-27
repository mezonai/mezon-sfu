#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

#include "pipeline/paced_send.h"

static void test_enqueue_spacing_and_copy(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);

  uint8_t payload[1000];
  memset(payload, 0x5a, sizeof(payload));
  struct sockaddr_storage dst;
  memset(&dst, 0, sizeof(dst));

  int64_t first_release = -1;
  int64_t second_release = -1;
  assert(sfu_paced_send_enqueue(&q, payload, sizeof(payload), &dst, sizeof(struct sockaddr_in), 1000000, 1000000, &first_release));
  payload[0] = 0;
  assert(sfu_paced_send_enqueue(&q, payload, sizeof(payload), &dst, sizeof(struct sockaddr_in), 1000000, 1000000, &second_release));

  assert(q.count == 2);
  assert(first_release == 1000000);
  assert(second_release == 1004000);
  assert(q.entries[q.head].data[0] == 0x5a);

  sfu_paced_send_destroy(&q);
}

static void test_rate_floor_and_size_limit(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);

  uint8_t payload[SFU_PACED_SEND_MAX_PAYLOAD + 1];
  struct sockaddr_storage dst;
  memset(&dst, 0, sizeof(dst));

  int64_t first_release = -1;
  int64_t second_release = -1;
  assert(!sfu_paced_send_enqueue(&q, payload, sizeof(payload), &dst, sizeof(struct sockaddr_in), 1000000, 0, &first_release));
  assert(sfu_paced_send_enqueue(&q, payload, 1000, &dst, sizeof(struct sockaddr_in), 1, 2000000, &first_release));
  assert(sfu_paced_send_enqueue(&q, payload, 1000, &dst, sizeof(struct sockaddr_in), 1, 2000000, &second_release));
  assert(first_release == 2000000);
  assert(second_release == 2004000);

  sfu_paced_send_destroy(&q);
}

static void test_frame_admission(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);

  q.count = 1;
  q.next_release_us = 1199999;
  assert(sfu_paced_send_admit_frame_packet(&q, 10, false, false, 1000000));
  assert(sfu_paced_send_admit_frame_packet(&q, 10, true, false, 1000000));

  q.next_release_us = 1200000;
  assert(!sfu_paced_send_admit_frame_packet(&q, 11, false, false, 1000000));
  assert(!sfu_paced_send_admit_frame_packet(&q, 11, true, false, 1000000));
  assert(q.dropped_delay_frames == 1);
  assert(q.dropped_frame_packets == 2);

  assert(sfu_paced_send_admit_frame_packet(&q, 12, true, false, 1200000));
  q.next_release_us = 1300000;
  assert(sfu_paced_send_admit_frame_packet(&q, 13, true, true, 1000000));

  sfu_paced_send_destroy(&q);
}

static void test_projected_delay(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);
  assert(sfu_paced_send_projected_delay_us(&q, 1000) == 0);
  q.count = 1;
  q.next_release_us = 5000;
  assert(sfu_paced_send_projected_delay_us(&q, 2000) == 3000);
  assert(sfu_paced_send_projected_delay_us(&q, 6000) == 0);
  sfu_paced_send_destroy(&q);
}

static void test_large_frame_serialization_bound(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);

  uint8_t payload[1200];
  memset(payload, 0x42, sizeof(payload));
  struct sockaddr_storage dst;
  memset(&dst, 0, sizeof(dst));

  const uint32_t frame_bytes = 42291;
  uint32_t remaining = frame_bytes;
  int64_t now_us = 3000000;
  int64_t release_at_us = 0;
  while (remaining > 0) {
    uint16_t len = remaining > sizeof(payload) ? (uint16_t)sizeof(payload) : (uint16_t)remaining;
    assert(sfu_paced_send_enqueue(&q, payload, len, &dst, sizeof(struct sockaddr_in), 1, now_us, &release_at_us));
    remaining -= len;
  }

  assert(q.count == 36);
  assert(q.next_release_us - now_us <= SFU_PACED_SEND_MAX_DELAY_US);
  assert(q.next_release_us - now_us >= 169164);

  sfu_paced_send_destroy(&q);
}

static void test_rate_above_floor(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);

  uint8_t payload[1000] = {0};
  struct sockaddr_storage dst;
  memset(&dst, 0, sizeof(dst));
  int64_t first_release = 0;
  int64_t second_release = 0;
  assert(sfu_paced_send_enqueue(&q, payload, sizeof(payload), &dst, sizeof(struct sockaddr_in), 4000000, 4000000, &first_release));
  assert(sfu_paced_send_enqueue(&q, payload, sizeof(payload), &dst, sizeof(struct sockaddr_in), 4000000, 4000000, &second_release));
  assert(first_release == 4000000);
  assert(second_release == 4002000);

  sfu_paced_send_destroy(&q);
}

static void test_frame_input_span(void) {
  sfu_paced_send_t q;
  sfu_paced_send_init(&q);

  assert(sfu_paced_send_admit_frame_packet(&q, 20, false, false, 1000000));
  assert(sfu_paced_send_admit_frame_packet(&q, 20, true, false, 1250000));
  assert(q.max_input_frame_span_us == 250000);
  assert(q.input_frames_over_delay == 1);

  assert(sfu_paced_send_admit_frame_packet(&q, 21, false, false, 1300000));
  assert(sfu_paced_send_admit_frame_packet(&q, 22, true, false, 1550000));
  assert(q.max_input_frame_span_us == 250000);
  assert(q.input_frames_over_delay == 2);

  sfu_paced_send_destroy(&q);
}

int main(void) {
  test_enqueue_spacing_and_copy();
  test_rate_floor_and_size_limit();
  test_frame_admission();
  test_projected_delay();
  test_large_frame_serialization_bound();
  test_rate_above_floor();
  test_frame_input_span();
  return 0;
}
