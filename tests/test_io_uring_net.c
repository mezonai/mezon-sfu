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

static void test_selection(void) {
  sfu_net_send_priority_t input[] = {SFU_NET_PRIORITY_NORMAL, SFU_NET_PRIORITY_CONTROL, SFU_NET_PRIORITY_CONTROL,
                                     SFU_NET_PRIORITY_CONTROL, SFU_NET_PRIORITY_CONTROL, SFU_NET_PRIORITY_CONTROL,
                                     SFU_NET_PRIORITY_NORMAL, SFU_NET_PRIORITY_CONTROL};
  sfu_net_send_priority_t output[8];
  assert(sfu_net_test_select_priorities(input, 8, output, 8) == 8);
  sfu_net_send_priority_t expected[] = {SFU_NET_PRIORITY_CONTROL, SFU_NET_PRIORITY_CONTROL, SFU_NET_PRIORITY_CONTROL,
                                        SFU_NET_PRIORITY_CONTROL, SFU_NET_PRIORITY_NORMAL, SFU_NET_PRIORITY_CONTROL,
                                        SFU_NET_PRIORITY_CONTROL, SFU_NET_PRIORITY_NORMAL};
  assert(memcmp(output, expected, sizeof(expected)) == 0);
}

int main(void) {
  test_selection();
  sfu_net_options_t options = {.fd = -1, .send_entries = 2, .completion_entries = 8};
  sfu_net_t *net = sfu_net_create(&options);
  if (!net) {
    puts("test_io_uring_net: SKIP (io_uring unavailable)");
    return 0;
  }
  struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(7000)};
  uint8_t data[70] = {0};
  sfu_packet_t packets[70];
  for (unsigned i = 0; i < 70; i++) init_packet(&packets[i], &data[i]);

  assert(sfu_net_send(net, &packets[0], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[1], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[2], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[3], (struct sockaddr *)&dst, sizeof(dst)) == 0);
  assert(sfu_net_send(net, &packets[4], (struct sockaddr *)&dst, sizeof(dst)) == -1);
  assert(sfu_net_outstanding_sends(net) == 4);
  for (unsigned i = 0; i < 4; i++) assert(atomic_load_explicit(&packets[i].refcount, memory_order_relaxed) == 2);
  assert(atomic_load_explicit(&packets[4].refcount, memory_order_relaxed) == 1);
  struct sockaddr_in first = dst;
  struct sockaddr_in second = dst;
  first.sin_port = htons(7001);
  second.sin_port = htons(7002);
  assert(sfu_net_send_ex(net, &packets[4], (struct sockaddr *)&first, sizeof(first), SFU_NET_PRIORITY_CONTROL) == 0);
  assert(sfu_net_send_ex(net, &packets[4], (struct sockaddr *)&second, sizeof(second), SFU_NET_PRIORITY_CONTROL) == 0);
  struct sockaddr_storage copied;
  socklen_t copied_len = 0;
  sfu_packet_t *queued = NULL;
  assert(sfu_net_test_pending_at(net, SFU_NET_PRIORITY_CONTROL, 0, &queued, &copied, &copied_len));
  assert(queued == &packets[4] && copied_len == sizeof(first));
  assert(((struct sockaddr_in *)&copied)->sin_port == first.sin_port);
  assert(sfu_net_test_pending_at(net, SFU_NET_PRIORITY_CONTROL, 1, &queued, &copied, &copied_len));
  assert(((struct sockaddr_in *)&copied)->sin_port == second.sin_port);
  for (unsigned i = 5; i < 67; i++)
    assert(sfu_net_send_ex(net, &packets[i], (struct sockaddr *)&dst, sizeof(dst), SFU_NET_PRIORITY_CONTROL) == 0);
  assert(sfu_net_send_ex(net, &packets[67], (struct sockaddr *)&dst, sizeof(dst), SFU_NET_PRIORITY_CONTROL) == -1);
  assert(atomic_load_explicit(&packets[67].refcount, memory_order_relaxed) == 1);
  assert(sfu_net_outstanding_sends(net) == 68);

  assert(sfu_net_cancel(net) == 66);
  assert(sfu_net_outstanding_sends(net) == 2);
  assert(atomic_load_explicit(&packets[2].refcount, memory_order_relaxed) == 1);
  assert(atomic_load_explicit(&packets[3].refcount, memory_order_relaxed) == 1);
  sfu_net_destroy(net);
  puts("test_io_uring_net: OK");
  return 0;
}
