#include "memory/refcount.h"
#include "net/io_backend.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void init_packet(sfu_packet_t *pkt, uint8_t *data, uint32_t len) {
  memset(pkt, 0, sizeof(*pkt));
  pkt->data = data;
  pkt->len = len;
  pkt->cap = len;
  atomic_store_explicit(&pkt->refcount, 1, memory_order_relaxed);
}

static struct sockaddr_in destination(void) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(7000);
  assert(inet_pton(AF_INET, "192.0.2.20", &addr.sin_addr) == 1);
  return addr;
}

static void test_init_and_validation(void) {
  sfu_ring_t ring;
  assert(sfu_ring_init(&ring, -1, 3, 8, 0, 0, 0, false) == 0);
  assert(ring.queues_initialized);
  assert(ring.queue_capacity == 4096);
  assert(sfu_ring_submit(&ring) == 0);
  assert(sfu_ring_arm_recv(&ring) == -1);
  assert(sfu_ring_reap(&ring, 1, NULL, NULL, NULL, NULL, NULL) == 0);
  assert(sfu_ring_backend_service(NULL, &ring, 1, 1) == 0);
  assert(sfu_ring_outstanding_sends(&ring) == 0);
  sfu_ring_destroy(&ring);
  sfu_ring_destroy(&ring);
}

static void test_enqueue_and_queue_full(void) {
  sfu_ring_t ring;
  assert(sfu_ring_init(&ring, -1, 2, 2, 0, 0, 0, false) == 0);
  assert(ring.queue_capacity == 2);

  uint8_t data[3][16] = {{0}};
  sfu_packet_t packets[3];
  for (size_t i = 0; i < 3; i++) {
    init_packet(&packets[i], data[i], sizeof(data[i]));
  }
  struct sockaddr_in dst = destination();
  struct sockaddr_in6 dst6;
  memset(&dst6, 0, sizeof(dst6));
  dst6.sin6_family = AF_INET6;

  assert(sfu_ring_queue_send_zc(NULL, &packets[0], (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(sfu_ring_queue_send_zc(&ring, NULL, (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(sfu_ring_queue_send_zc(&ring, &packets[0], NULL, sizeof(dst)) == -1);
  assert(sfu_ring_queue_send_zc(&ring, &packets[0], (struct sockaddr *)&dst6, sizeof(dst6)) == -1);
  assert(sfu_ring_queue_send_zc(&ring, &packets[0], (struct sockaddr *)&dst, sizeof(dst) - 1) == -1);
  assert(atomic_load_explicit(&packets[0].refcount, memory_order_relaxed) == 1);

  assert(sfu_ring_queue_send_zc(&ring, &packets[0], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_ring_queue_send_zc(&ring, &packets[1], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_ring_outstanding_sends(&ring) == 2);
  assert(atomic_load_explicit(&packets[0].refcount, memory_order_relaxed) == 2);
  assert(atomic_load_explicit(&packets[1].refcount, memory_order_relaxed) == 2);
  assert(packets[0].peer_addr_len == sizeof(dst));

  assert(sfu_ring_queue_send_zc(&ring, &packets[2], (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(atomic_load_explicit(&packets[2].refcount, memory_order_relaxed) == 1);
  assert(sfu_ring_outstanding_sends(&ring) == 2);

  void *item = NULL;
  assert(sfu_spsc_ring_pop(&ring.tx_pending, &item) && item == &packets[0]);
  assert(sfu_packet_release(&packets[0]) == 0);
  assert(sfu_spsc_ring_pop(&ring.tx_pending, &item) && item == &packets[1]);
  assert(sfu_packet_release(&packets[1]) == 0);
  assert(!sfu_spsc_ring_pop(&ring.tx_pending, &item));

  sfu_ring_destroy(&ring);
}

int main(void) {
  test_init_and_validation();
  test_enqueue_and_queue_full();
  printf("test_af_xdp_ring: OK\n");
  return 0;
}
