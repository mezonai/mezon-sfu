#include "net/af_xdp_frame.h"

#include <arpa/inet.h>
#include <assert.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <stdio.h>
#include <string.h>

#define TEST_PORT 7000u
#define FRAME_CAPACITY 4096u

static const uint8_t k_source_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t k_destination_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

static sfu_af_xdp_frame_params_t make_params(const uint8_t *payload, uint32_t payload_len) {
  sfu_af_xdp_frame_params_t params;
  memset(&params, 0, sizeof(params));
  memcpy(params.source_mac, k_source_mac, sizeof(params.source_mac));
  memcpy(params.destination_mac, k_destination_mac, sizeof(params.destination_mac));
  assert(inet_pton(AF_INET, "192.0.2.10", &params.source_ip) == 1);
  assert(inet_pton(AF_INET, "198.51.100.20", &params.destination_ip) == 1);
  params.source_port = htons(50000);
  params.destination_port = htons(TEST_PORT);
  params.payload = payload;
  params.payload_len = payload_len;
  return params;
}

static uint32_t build_fixture(uint8_t *frame, uint32_t capacity, const uint8_t *payload, uint32_t payload_len) {
  sfu_af_xdp_frame_params_t params = make_params(payload, payload_len);
  uint32_t frame_len = 0;
  assert(sfu_af_xdp_build_frame(frame, capacity, &params, &frame_len));
  return frame_len;
}

static void test_checksum(void) {
  uint8_t odd[] = {0x01, 0x02, 0x03};
  assert(ntohs(sfu_af_xdp_checksum(odd, sizeof(odd))) == 0xfbfd);

  uint8_t unaligned[] = {0xff, 0x01, 0x02, 0x03};
  assert(ntohs(sfu_af_xdp_checksum(unaligned + 1, 3)) == 0xfbfd);
  assert(sfu_af_xdp_checksum(NULL, 0) == htons(0xffff));
  assert(sfu_af_xdp_checksum(NULL, 1) == 0);
}

static void test_build_and_parse(void) {
  uint8_t payload[1200];
  uint8_t frame[FRAME_CAPACITY];
  for (uint32_t i = 0; i < sizeof(payload); i++) {
    payload[i] = (uint8_t)i;
  }

  uint32_t frame_len = build_fixture(frame, sizeof(frame), payload, sizeof(payload));
  assert(frame_len == SFU_AF_XDP_FRAME_HEADER_SIZE + sizeof(payload));
  assert(memcmp(frame, k_destination_mac, 6) == 0);
  assert(memcmp(frame + 6, k_source_mac, 6) == 0);

  struct iphdr ip;
  memcpy(&ip, frame + SFU_AF_XDP_ETH_HEADER_SIZE, sizeof(ip));
  assert(ip.version == 4 && ip.ihl == 5 && ip.protocol == IPPROTO_UDP && ip.ttl == 64);
  assert(sfu_af_xdp_checksum(&ip, sizeof(ip)) == 0);

  sfu_af_xdp_parse_result_t parsed;
  assert(sfu_af_xdp_parse_frame(frame, frame_len, sizeof(frame), TEST_PORT, &parsed));
  assert(!parsed.vlan);
  assert(parsed.header_len == SFU_AF_XDP_FRAME_HEADER_SIZE);
  assert(parsed.payload_len == sizeof(payload));
  assert(parsed.payload_cap == sizeof(frame) - SFU_AF_XDP_FRAME_HEADER_SIZE);
  assert(parsed.source_port == htons(50000));
  assert(memcmp(parsed.payload, payload, sizeof(payload)) == 0);

  struct in_addr expected_source;
  assert(inet_pton(AF_INET, "192.0.2.10", &expected_source) == 1);
  assert(parsed.source_ip.s_addr == expected_source.s_addr);
}

static void test_vlan_and_ip_options(void) {
  uint8_t payload[32];
  uint8_t plain[FRAME_CAPACITY];
  uint8_t vlan[FRAME_CAPACITY];
  memset(payload, 0xa5, sizeof(payload));
  uint32_t plain_len = build_fixture(plain, sizeof(plain), payload, sizeof(payload));

  memcpy(vlan, plain, 12);
  uint16_t vlan_proto = htons(ETH_P_8021Q);
  uint16_t vlan_tci = htons(7);
  uint16_t ip_proto = htons(ETH_P_IP);
  memcpy(vlan + 12, &vlan_proto, 2);
  memcpy(vlan + 14, &vlan_tci, 2);
  memcpy(vlan + 16, &ip_proto, 2);
  memcpy(vlan + 18, plain + 14, plain_len - 14);

  sfu_af_xdp_parse_result_t parsed;
  assert(sfu_af_xdp_parse_frame(vlan, plain_len + 4, sizeof(vlan), TEST_PORT, &parsed));
  assert(parsed.vlan);
  assert(parsed.header_len == SFU_AF_XDP_FRAME_HEADER_SIZE + 4);
  assert(parsed.payload_len == sizeof(payload));

  uint8_t options[FRAME_CAPACITY];
  memcpy(options, plain, SFU_AF_XDP_ETH_HEADER_SIZE + SFU_AF_XDP_IPV4_HEADER_SIZE);
  memmove(options + SFU_AF_XDP_ETH_HEADER_SIZE + 24, plain + SFU_AF_XDP_ETH_HEADER_SIZE + 20, plain_len - SFU_AF_XDP_ETH_HEADER_SIZE - 20);
  memset(options + SFU_AF_XDP_ETH_HEADER_SIZE + 20, 0, 4);
  struct iphdr ip;
  memcpy(&ip, options + SFU_AF_XDP_ETH_HEADER_SIZE, sizeof(ip));
  ip.ihl = 6;
  ip.tot_len = htons((uint16_t)(ntohs(ip.tot_len) + 4));
  ip.check = 0;
  memcpy(options + SFU_AF_XDP_ETH_HEADER_SIZE, &ip, sizeof(ip));
  uint16_t checksum = sfu_af_xdp_checksum(options + SFU_AF_XDP_ETH_HEADER_SIZE, 24);
  memcpy(options + SFU_AF_XDP_ETH_HEADER_SIZE + 10, &checksum, sizeof(checksum));
  assert(sfu_af_xdp_parse_frame(options, plain_len + 4, sizeof(options), TEST_PORT, &parsed));
  assert(parsed.header_len == SFU_AF_XDP_FRAME_HEADER_SIZE + 4);
}

static void test_rejections(void) {
  uint8_t payload[16];
  uint8_t frame[FRAME_CAPACITY];
  memset(payload, 0x5a, sizeof(payload));
  uint32_t frame_len = build_fixture(frame, sizeof(frame), payload, sizeof(payload));
  sfu_af_xdp_parse_result_t parsed;

  assert(!sfu_af_xdp_parse_frame(NULL, frame_len, sizeof(frame), TEST_PORT, &parsed));
  assert(!sfu_af_xdp_parse_frame(frame, frame_len, sizeof(frame), TEST_PORT, NULL));
  assert(!sfu_af_xdp_parse_frame(frame, frame_len, frame_len - 1, TEST_PORT, &parsed));
  assert(!sfu_af_xdp_parse_frame(frame, 13, sizeof(frame), TEST_PORT, &parsed));
  assert(!sfu_af_xdp_parse_frame(frame, frame_len, sizeof(frame), TEST_PORT + 1, &parsed));

  uint8_t broken[FRAME_CAPACITY];
  memcpy(broken, frame, frame_len);
  uint16_t non_ip = htons(ETH_P_ARP);
  memcpy(broken + 12, &non_ip, sizeof(non_ip));
  assert(!sfu_af_xdp_parse_frame(broken, frame_len, sizeof(broken), TEST_PORT, &parsed));

  memcpy(broken, frame, frame_len);
  broken[SFU_AF_XDP_ETH_HEADER_SIZE] = 0x65;
  assert(!sfu_af_xdp_parse_frame(broken, frame_len, sizeof(broken), TEST_PORT, &parsed));

  memcpy(broken, frame, frame_len);
  uint16_t fragment = htons(0x2000);
  memcpy(broken + SFU_AF_XDP_ETH_HEADER_SIZE + 6, &fragment, sizeof(fragment));
  assert(!sfu_af_xdp_parse_frame(broken, frame_len, sizeof(broken), TEST_PORT, &parsed));

  memcpy(broken, frame, frame_len);
  uint16_t short_total = htons(20);
  memcpy(broken + SFU_AF_XDP_ETH_HEADER_SIZE + 2, &short_total, sizeof(short_total));
  assert(!sfu_af_xdp_parse_frame(broken, frame_len, sizeof(broken), TEST_PORT, &parsed));

  memcpy(broken, frame, frame_len);
  uint16_t oversized_udp = htons(4096);
  memcpy(broken + SFU_AF_XDP_ETH_HEADER_SIZE + SFU_AF_XDP_IPV4_HEADER_SIZE + 4, &oversized_udp, sizeof(oversized_udp));
  assert(!sfu_af_xdp_parse_frame(broken, frame_len, sizeof(broken), TEST_PORT, &parsed));

  memcpy(broken, frame, frame_len);
  uint16_t empty_udp = htons(SFU_AF_XDP_UDP_HEADER_SIZE);
  memcpy(broken + SFU_AF_XDP_ETH_HEADER_SIZE + SFU_AF_XDP_IPV4_HEADER_SIZE + 4, &empty_udp, sizeof(empty_udp));
  assert(!sfu_af_xdp_parse_frame(broken, frame_len, sizeof(broken), TEST_PORT, &parsed));
}

static void test_build_capacity(void) {
  uint8_t payload[64] = {0};
  uint8_t frame[128];
  uint32_t out_len = 123;
  sfu_af_xdp_frame_params_t params = make_params(payload, sizeof(payload));

  assert(!sfu_af_xdp_build_frame(NULL, sizeof(frame), &params, &out_len));
  assert(!sfu_af_xdp_build_frame(frame, sizeof(frame), NULL, &out_len));
  assert(!sfu_af_xdp_build_frame(frame, sizeof(frame), &params, NULL));
  assert(!sfu_af_xdp_build_frame(frame, SFU_AF_XDP_FRAME_HEADER_SIZE + sizeof(payload) - 1, &params, &out_len));

  params.payload = NULL;
  assert(!sfu_af_xdp_build_frame(frame, sizeof(frame), &params, &out_len));
  params.payload_len = 0;
  assert(!sfu_af_xdp_build_frame(frame, sizeof(frame), &params, &out_len));
}

int main(void) {
  test_checksum();
  test_build_and_parse();
  test_vlan_and_ip_options();
  test_rejections();
  test_build_capacity();
  printf("test_af_xdp_frame: OK\n");
  return 0;
}
