#include "rtp/rtp_packet.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_minimal_header(void) {
  const uint8_t data[12] = {0x80, 0xe0, 0x12, 0x34, 0x01, 0x23, 0x45, 0x67,
                            0x89, 0xab, 0xcd, 0xef};
  sfu_rtp_packet_t packet;
  assert(sfu_rtp_packet_parse(data, sizeof(data), &packet));
  assert(packet.version == 2 && !packet.padding && !packet.extension);
  assert(packet.csrc_count == 0 && packet.marker && packet.payload_type == 96);
  assert(packet.sequence_number == 0x1234 && packet.timestamp == 0x01234567u);
  assert(packet.ssrc == 0x89abcdefu && packet.header_len == 12);
  assert(packet.payload == data + 12 && packet.payload_len == 0);
}

static void test_csrcs(void) {
  const uint8_t data[21] = {0x82, 0x61, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3,
                            0, 0, 0, 4, 0, 0, 0, 5, 0xaa};
  sfu_rtp_packet_t packet;
  assert(sfu_rtp_packet_parse(data, sizeof(data), &packet));
  assert(packet.csrc_count == 2 && packet.header_len == 20);
  assert(packet.payload == data + 20 && packet.payload_len == 1);
}

static void test_extension(void) {
  const uint8_t data[23] = {0x90, 0x60, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3,
                            0xbe, 0xde, 0, 1, 1, 2, 3, 4, 0xaa, 0xbb, 0xcc};
  sfu_rtp_packet_t packet;
  assert(sfu_rtp_packet_parse(data, sizeof(data), &packet));
  assert(packet.extension && packet.extension_profile == 0xbede);
  assert(packet.extension_data == data + 16 && packet.extension_length == 4);
  assert(packet.header_len == 20 && packet.payload_len == 3);
}

static void test_malformed(void) {
  uint8_t data[32] = {0x80, 0x60};
  sfu_rtp_packet_t packet;
  assert(!sfu_rtp_packet_parse(data, 11, &packet));
  data[0] = 0x40;
  assert(!sfu_rtp_packet_parse(data, 12, &packet));
  data[0] = 0x8f;
  assert(!sfu_rtp_packet_parse(data, 12, &packet));

  data[0] = 0x90;
  assert(!sfu_rtp_packet_parse(data, 15, &packet));
  memset(data, 0, sizeof(data));
  data[0] = 0x90;
  data[14] = 0;
  data[15] = 5;
  assert(!sfu_rtp_packet_parse(data, 20, &packet));
  assert(!sfu_rtp_packet_parse(NULL, 12, &packet));
  assert(!sfu_rtp_packet_parse(data, 20, NULL));
}

static void test_padding(void) {
  uint8_t data[17] = {0xa0, 0x60, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3,
                      0xaa, 0xbb, 0, 0, 3};
  sfu_rtp_packet_t packet;
  assert(sfu_rtp_packet_parse(data, sizeof(data), &packet));
  assert(packet.padding && packet.payload_len == 2);
  data[16] = 0;
  assert(!sfu_rtp_packet_parse(data, sizeof(data), &packet));
  data[16] = 6;
  assert(!sfu_rtp_packet_parse(data, sizeof(data), &packet));
  assert(!sfu_rtp_packet_parse(data, 12, &packet));
}

static void test_unaligned_and_editors(void) {
  uint8_t *raw = malloc(32);
  uint8_t *data;
  sfu_rtp_packet_t packet;
  assert(raw != NULL);
  data = raw + 1;
  memset(data, 0, 16);
  data[0] = 0x80;
  data[1] = 0x80;
  assert(sfu_rtp_packet_set_pt(data, 12, 111));
  assert(sfu_rtp_packet_set_marker(data, 12, false));
  assert(sfu_rtp_packet_set_seq(data, 12, 0xa1b2));
  assert(sfu_rtp_packet_set_ssrc(data, 12, 0x12345678u));
  assert(sfu_rtp_packet_parse(data, 12, &packet));
  assert(!packet.marker && packet.payload_type == 111);
  assert(sfu_rtp_packet_set_marker(data, 12, true));
  assert(sfu_rtp_packet_parse(data, 12, &packet));
  assert(packet.marker);
  assert(packet.sequence_number == 0xa1b2 && packet.ssrc == 0x12345678u);
  assert(!sfu_rtp_packet_set_pt(data, 11, 1));
  assert(!sfu_rtp_packet_set_pt(data, 12, 128));
  assert(!sfu_rtp_packet_set_marker(data, 11, true));
  assert(!sfu_rtp_packet_set_seq(NULL, 12, 1));
  assert(!sfu_rtp_packet_set_ssrc(data, 11, 1));
  free(raw);
}

int main(void) {
  test_minimal_header();
  test_csrcs();
  test_extension();
  test_malformed();
  test_padding();
  test_unaligned_and_editors();
  printf("test_rtp_packet: OK\n");
  return 0;
}
