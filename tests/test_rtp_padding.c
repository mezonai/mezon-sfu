#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "rtp/rtp_ext.h"
#include "rtp/rtp_packet.h"
#include "rtp/rtp_padding.h"

static void test_rtp_padding_basic_no_ext(void) {
  uint8_t buf[512] = {0};
  size_t len = 0;
  uint8_t pt = 97;
  uint16_t seq = 12345;
  uint32_t ts = 987654321;
  uint32_t ssrc = 0x11223344;
  uint16_t pad_bytes = 100;

  assert(sfu_rtp_padding_build(buf, sizeof(buf), pt, seq, ts, ssrc, pad_bytes, 0, 0, &len));
  assert(len == 12 + 100);

  sfu_rtp_packet_t pkt;
  assert(sfu_rtp_packet_parse(buf, len, &pkt));
  assert(pkt.version == 2);
  assert(pkt.padding == true);
  assert(pkt.extension == false);
  assert(pkt.payload_type == pt);
  assert(pkt.sequence_number == seq);
  assert(pkt.timestamp == ts);
  assert(pkt.ssrc == ssrc);
  assert(pkt.header_len == 12);
  assert(pkt.payload_len == 0);

  /* Last byte must be padding count */
  assert(buf[len - 1] == 100);
  /* Preceding padding bytes should be zero */
  for (size_t i = 12; i < len - 1; i++) {
    assert(buf[i] == 0);
  }
}

static void test_rtp_padding_with_twcc_ext(void) {
  uint8_t buf[512] = {0};
  size_t len = 0;
  uint8_t pt = 99;
  uint16_t seq = 54321;
  uint32_t ts = 123456789;
  uint32_t ssrc = 0x55667788;
  uint16_t pad_bytes = 224;
  uint8_t twcc_id = 5;
  uint16_t twcc_seq = 4242;

  assert(sfu_rtp_padding_build(buf, sizeof(buf), pt, seq, ts, ssrc, pad_bytes, twcc_id, twcc_seq, &len));
  /* 12 fixed + 8 extension + 224 padding = 244 bytes */
  assert(len == 12 + 8 + 224);

  sfu_rtp_packet_t pkt;
  assert(sfu_rtp_packet_parse(buf, len, &pkt));
  assert(pkt.version == 2);
  assert(pkt.padding == true);
  assert(pkt.extension == true);
  assert(pkt.payload_type == pt);
  assert(pkt.sequence_number == seq);
  assert(pkt.timestamp == ts);
  assert(pkt.ssrc == ssrc);
  assert(pkt.header_len == 20);
  assert(pkt.payload_len == 0);

  /* Verify TWCC extension can be read back */
  uint16_t read_twcc_seq = 0;
  assert(sfu_rtp_ext_read_twcc(pkt.extension_profile, pkt.extension_data, pkt.extension_length, twcc_id, &read_twcc_seq));
  assert(read_twcc_seq == twcc_seq);

  /* Last byte must be padding count */
  assert(buf[len - 1] == 224);
}

static void test_rtp_padding_boundary_limits(void) {
  uint8_t buf[512] = {0};
  size_t len = 0;

  /* 1 byte padding (minimum valid) */
  assert(sfu_rtp_padding_build(buf, sizeof(buf), 96, 1, 1, 1, 1, 0, 0, &len));
  assert(len == 13);
  assert(buf[len - 1] == 1);
  sfu_rtp_packet_t pkt;
  assert(sfu_rtp_packet_parse(buf, len, &pkt));
  assert(pkt.payload_len == 0);

  /* 255 bytes padding (maximum valid for 1-octet count) */
  assert(sfu_rtp_padding_build(buf, sizeof(buf), 96, 1, 1, 1, 255, 0, 0, &len));
  assert(len == 12 + 255);
  assert(buf[len - 1] == 255);
  assert(sfu_rtp_packet_parse(buf, len, &pkt));
  assert(pkt.payload_len == 0);

  /* Default padding size when padding_bytes == 0 */
  assert(sfu_rtp_padding_build(buf, sizeof(buf), 96, 1, 1, 1, 0, 0, 0, &len));
  assert(len == 12 + SFU_RTP_PADDING_DEFAULT_BYTES);
  assert(buf[len - 1] == SFU_RTP_PADDING_DEFAULT_BYTES);

  /* Exceeding 255 bytes must fail */
  assert(!sfu_rtp_padding_build(buf, sizeof(buf), 96, 1, 1, 1, 256, 0, 0, &len));
}

static void test_rtp_padding_capacity_and_errors(void) {
  uint8_t buf[32] = {0};
  size_t len = 0;

  /* Buffer too small for fixed header + padding */
  assert(!sfu_rtp_padding_build(buf, 20, 96, 1, 1, 1, 10, 0, 0, &len));

  /* Buffer too small for header + TWCC extension + padding */
  assert(!sfu_rtp_padding_build(buf, 25, 96, 1, 1, 1, 10, 1, 1, &len));

  /* NULL buffer or out_len */
  assert(!sfu_rtp_padding_build(NULL, 100, 96, 1, 1, 1, 10, 0, 0, &len));
  assert(!sfu_rtp_padding_build(buf, 100, 96, 1, 1, 1, 10, 0, 0, NULL));

  /* Invalid PT (> 127) */
  assert(!sfu_rtp_padding_build(buf, sizeof(buf), 128, 1, 1, 1, 10, 0, 0, &len));

  /* Invalid SSRC (0) */
  assert(!sfu_rtp_padding_build(buf, sizeof(buf), 96, 1, 1, 0, 10, 0, 0, &len));

  /* Invalid TWCC ID (> 14) */
  assert(!sfu_rtp_padding_build(buf, sizeof(buf), 96, 1, 1, 1, 10, 15, 0, &len));
}

int main(void) {
  test_rtp_padding_basic_no_ext();
  test_rtp_padding_with_twcc_ext();
  test_rtp_padding_boundary_limits();
  test_rtp_padding_capacity_and_errors();
  return 0;
}
