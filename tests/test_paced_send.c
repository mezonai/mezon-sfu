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

int main(void) {
  test_enqueue_spacing_and_copy();
  test_rate_floor_and_size_limit();
  return 0;
}
