#include "memory/refcount.h"
#include "net/af_xdp_frame.h"
#include "net/net.h"

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

static void test_create_and_validation(void) {
  sfu_net_options_t options = {.fd = -1, .send_entries = 3, .completion_entries = 8};
  sfu_net_t *net = sfu_net_create(&options);
  assert(net != NULL);
  assert(sfu_net_flush(net) == 0);
  assert(sfu_net_recv(net) == -1);
  assert(sfu_net_poll(net, 1, NULL, NULL, NULL, NULL, NULL) == 0);
  assert(sfu_net_service(NULL, net, 1) == 0);
  assert(sfu_net_outstanding_sends(net) == 0);
  sfu_net_destroy(net);
}

static void test_enqueue_and_queue_full(void) {
  sfu_net_options_t options = {.fd = -1, .send_entries = 2, .completion_entries = 2};
  sfu_net_t *net = sfu_net_create(&options);
  assert(net != NULL);

  uint8_t data[3][16] = {{0}};
  sfu_packet_t packets[3];
  for (size_t i = 0; i < 3; i++) {
    init_packet(&packets[i], data[i], sizeof(data[i]));
  }
  struct sockaddr_in dst = destination();
  struct sockaddr_in6 dst6;
  memset(&dst6, 0, sizeof(dst6));
  dst6.sin6_family = AF_INET6;

  assert(sfu_net_send(NULL, &packets[0], (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(sfu_net_send(net, NULL, (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(sfu_net_send(net, &packets[0], NULL, sizeof(dst)) == -1);
  assert(sfu_net_send(net, &packets[0], (struct sockaddr *)&dst6, sizeof(dst6)) == -1);
  assert(sfu_net_send(net, &packets[0], (struct sockaddr *)&dst, sizeof(dst) - 1) == -1);
  assert(atomic_load_explicit(&packets[0].refcount, memory_order_relaxed) == 1);

  assert(sfu_net_send(net, &packets[0], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[1], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_outstanding_sends(net) == 2);
  assert(atomic_load_explicit(&packets[0].refcount, memory_order_relaxed) == 2);
  assert(atomic_load_explicit(&packets[1].refcount, memory_order_relaxed) == 2);

  assert(sfu_net_send(net, &packets[2], (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(atomic_load_explicit(&packets[2].refcount, memory_order_relaxed) == 1);
  assert(sfu_net_outstanding_sends(net) == 2);

  assert(sfu_packet_release(&packets[0]) == 0);
  assert(sfu_packet_release(&packets[1]) == 0);
  sfu_net_destroy(net);
}

static void test_multi_queue_helpers(void) {
  assert(sfu_af_xdp_frames_per_queue(16384, 8) == 2048);
  assert(sfu_af_xdp_frames_per_queue(16384, 3) == 4096);
  assert(sfu_af_xdp_frames_per_queue(32, 8) == 0);
  assert(sfu_af_xdp_frames_per_queue(16384, 0) == 0);

  uintptr_t token = 0;
  uint32_t queue_slot = UINT32_MAX;
  uint32_t frame = UINT32_MAX;
  assert(sfu_af_xdp_encode_rx_return(2, 17, &token));
  assert(sfu_af_xdp_decode_rx_return(token, &queue_slot, &frame));
  assert(queue_slot == 2 && frame == 17);
  assert(!sfu_af_xdp_encode_rx_return(UINT32_MAX, 0, &token));
  assert(!sfu_af_xdp_decode_rx_return(0, &queue_slot, &frame));
}

int main(void) {
  test_create_and_validation();
  test_enqueue_and_queue_full();
  test_multi_queue_helpers();
  printf("test_af_xdp_net: OK\n");
  return 0;
}
