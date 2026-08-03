#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rtp/rtp_packet.h"
#include "rtp/rtx_build.h"
#include "util/netbytes.h"

static void test_minimal_and_editor_fields(void) {
  const uint8_t orig[] = {0x80, 0xe0, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04,
                          0x11, 0x22, 0x33, 0x44, 0xaa, 0xbb, 0xcc};
  const uint8_t expected[] = {0x80, 0xe1, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04,
                              0x55, 0x66, 0x77, 0x88, 0x12, 0x34, 0xaa, 0xbb,
                              0xcc};
  uint8_t out[sizeof(orig) + 2u];
  size_t out_len = 0;
  sfu_rtp_packet_t packet;

  assert(sfu_rtx_build(orig, sizeof(orig), 97, 0xbeef, 0x55667788u, out,
                       sizeof(out), &out_len));
  assert(out_len == sizeof(expected));
  assert(memcmp(out, expected, sizeof(expected)) == 0);
  assert(sfu_rtp_packet_parse(out, out_len, &packet));
  assert(packet.marker && packet.payload_type == 97);
  assert(packet.sequence_number == 0xbeef && packet.timestamp == 0x01020304u);
  assert(packet.ssrc == 0x55667788u && packet.payload_len == 5u);
}

static void test_csrc_extension_and_unaligned(void) {
  const uint8_t orig[] = {
      0x92, 0x62, 0x22, 0x33, 0x10, 0x20, 0x30, 0x40, 0x01, 0x02, 0x03, 0x04,
      0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44, 0xbe, 0xde, 0x00, 0x02,
      0x10, 0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23, 0xde, 0xad};
  uint8_t storage[sizeof(orig) + 4u];
  uint8_t *out = storage + 1u;
  size_t out_len = 99;
  sfu_rtp_packet_t packet;

  assert(sfu_rtx_build(orig, sizeof(orig), 99, 7, 0xa1a2a3a4u, out,
                       sizeof(orig) + 2u, &out_len));
  assert(out_len == sizeof(orig) + 2u);
  assert(sfu_rtp_packet_parse(out, out_len, &packet));
  assert(packet.header_len == 32u && packet.csrc_count == 2u);
  assert(packet.extension && packet.extension_profile == 0xbede);
  assert(packet.extension_length == 8u);
  assert(memcmp(out + 12u, orig + 12u, 20u) == 0);
  assert(sfu_read_be16(out + packet.header_len) == 0x2233);
  assert(out[packet.header_len + 2u] == 0xde && out[packet.header_len + 3u] == 0xad);
}

static void test_padding_stripped(void) {
  const uint8_t orig[] = {0xa0, 0x60, 0x00, 0x09, 0, 0, 0, 1, 0, 0, 0, 2,
                          0x41, 0x42, 0, 0, 0, 4};
  uint8_t out[sizeof(orig) + 2u];
  size_t out_len = 0;
  sfu_rtp_packet_t packet;

  assert(sfu_rtx_build(orig, sizeof(orig), 100, 10, 3, out, sizeof(out), &out_len));
  assert(out_len == 16u);
  assert((out[0] & 0x20u) == 0);
  assert(sfu_rtp_packet_parse(out, out_len, &packet));
  assert(!packet.padding && packet.payload_len == 4u);
  assert(sfu_read_be16(packet.payload) == 9u);
  assert(packet.payload[2] == 0x41 && packet.payload[3] == 0x42);
}

static void test_failures_and_capacity(void) {
  const uint8_t orig[] = {0x80, 0x60, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0xaa};
  const uint8_t malformed[] = {0x90, 0x60, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3};
  uint8_t out[sizeof(orig) + 2u];
  uint8_t canary[sizeof(out)];
  size_t out_len = 0xabcdefu;

  memset(out, 0xa5, sizeof(out));
  memcpy(canary, out, sizeof(out));
  assert(!sfu_rtx_build(orig, sizeof(orig), 98, 2, 4, out, sizeof(out) - 1u,
                        &out_len));
  assert(memcmp(out, canary, sizeof(out)) == 0 && out_len == 0xabcdefu);
  assert(!sfu_rtx_build(malformed, sizeof(malformed), 98, 2, 4, out,
                        sizeof(out), &out_len));
  assert(memcmp(out, canary, sizeof(out)) == 0 && out_len == 0xabcdefu);
  assert(sfu_rtx_build(orig, sizeof(orig), 98, 2, 4, out, sizeof(out), &out_len));
  assert(out_len == sizeof(out));
}

int main(void) {
  test_minimal_and_editor_fields();
  test_csrc_extension_and_unaligned();
  test_padding_stripped();
  test_failures_and_capacity();
  return 0;
}
