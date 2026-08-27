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
  assert(second_release == 1008000);
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
  assert(second_release == 2040000);

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

int main(void) {
  test_enqueue_spacing_and_copy();
  test_rate_floor_and_size_limit();
  test_frame_admission();
  test_projected_delay();
  return 0;
}
