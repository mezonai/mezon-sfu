#include "memory/refcount.h"
#include "net/net.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void init_packet(sfu_packet_t *pkt, uint8_t *data) {
  memset(pkt, 0, sizeof(*pkt));
  pkt->data = data;
  pkt->len = 1;
  pkt->cap = 1;
  atomic_store_explicit(&pkt->refcount, 1, memory_order_relaxed);
}

int main(void) {
  sfu_net_options_t options = {.fd = -1, .send_entries = 2, .completion_entries = 8};
  sfu_net_t *net = sfu_net_create(&options);
  if (!net) {
    puts("test_io_uring_net: SKIP (io_uring unavailable)");
    return 0;
  }
  struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(7000)};
  uint8_t data[5] = {0};
  sfu_packet_t packets[5];
  for (unsigned i = 0; i < 5; i++) init_packet(&packets[i], &data[i]);

  assert(sfu_net_send(net, &packets[0], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[1], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[2], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[3], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[4], (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(sfu_net_outstanding_sends(net) == 4);
  for (unsigned i = 0; i < 4; i++) assert(atomic_load_explicit(&packets[i].refcount, memory_order_relaxed) == 2);
  assert(atomic_load_explicit(&packets[4].refcount, memory_order_relaxed) == 1);

  assert(sfu_net_cancel(net) == 2);
  assert(sfu_net_outstanding_sends(net) == 2);
  assert(atomic_load_explicit(&packets[2].refcount, memory_order_relaxed) == 1);
  assert(atomic_load_explicit(&packets[3].refcount, memory_order_relaxed) == 1);
  sfu_net_destroy(net);
  puts("test_io_uring_net: OK");
  return 0;
}
