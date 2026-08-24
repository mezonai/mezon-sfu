#include "rtcp/rtcp_fb.h"
#include "rtcp/rtcp_kf.h"
#include "util/netbytes.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static sfu_rtcp_member_view view_of(const uint8_t *packet, size_t len) {
  sfu_rtcp_compound_iter iter;
  sfu_rtcp_member_view view;
  sfu_rtcp_compound_iter_init(&iter, packet, len);
  assert(sfu_rtcp_compound_iter_next(&iter, &view) == SFU_RTCP_COMPOUND_ITEM);
  return view;
}

static void test_pli(void) {
  const uint8_t packet[] = {0x81, 206, 0, 2, 0x11, 0x22, 0x33, 0x44,
                            0xaa, 0xbb, 0xcc, 0xdd};
  sfu_rtcp_member_view view = view_of(packet, sizeof(packet));
  sfu_rtcp_pli pli;
  assert(sfu_rtcp_parse_pli(&view, &pli));
  assert(pli.sender_ssrc == 0x11223344u && pli.media_ssrc == 0xaabbccddu);

  sfu_rtcp_member_view bad = view;
  bad.pt = 205; assert(!sfu_rtcp_parse_pli(&bad, &pli));
  bad = view; bad.fmt_count = 2; assert(!sfu_rtcp_parse_pli(&bad, &pli));
  bad = view; bad.member_len = 16; assert(!sfu_rtcp_parse_pli(&bad, &pli));
  bad = view; bad.body_len = 8; assert(!sfu_rtcp_parse_pli(&bad, &pli));
}

static void test_padding_logical_pli(void) {
  const uint8_t padded[] = {0xa1, 206, 0, 3, 0, 0, 0, 1,
                            0, 0, 0, 2, 0, 0, 0, 4};
  sfu_rtcp_member_view view = view_of(padded, sizeof(padded));
  sfu_rtcp_pli pli;
  assert(view.member_len == 12 && view.body_len == 4);
  assert(sfu_rtcp_parse_pli(&view, &pli) && pli.media_ssrc == 2);
}

static void test_fir(void) {
  const uint8_t packet[] = {
      0x84, 206, 0, 4, 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0,
      0xaa, 0xbb, 0xcc, 0xdd, 7, 0, 0, 0,
  };
  sfu_rtcp_member_view view = view_of(packet, sizeof(packet));
  sfu_rtcp_fir fir;
  sfu_rtcp_fir_entry entry;
  assert(sfu_rtcp_parse_fir(&view, &fir));
  assert(fir.sender_ssrc == 0x11223344u && fir.media_ssrc == 0 && fir.entry_count == 1);
  assert(sfu_rtcp_fir_entry_at(&fir, 0, &entry));
  assert(entry.target_ssrc == 0xaabbccddu && entry.sequence_number == 7);
  assert(!sfu_rtcp_fir_entry_at(&fir, 1, &entry));

  sfu_rtcp_member_view bad = view;
  bad.pt = 205; assert(!sfu_rtcp_parse_fir(&bad, &fir));
  bad = view; bad.fmt_count = 1; assert(!sfu_rtcp_parse_fir(&bad, &fir));
  bad = view; bad.member_len--; assert(!sfu_rtcp_parse_fir(&bad, &fir));
  bad = view; bad.body_len = 11; assert(!sfu_rtcp_parse_fir(&bad, &fir));

  uint8_t reserved[sizeof(packet)]; memcpy(reserved, packet, sizeof(packet));
  reserved[19] = 1; bad = view_of(reserved, sizeof(reserved));
  assert(!sfu_rtcp_parse_fir(&bad, &fir));
}

static void test_multiple_and_unaligned(void) {
  const uint8_t packet[] = {
      0x84, 206, 0, 6, 0, 0, 0, 1, 0, 0, 0, 0,
      0, 0, 0, 2, 9, 0, 0, 0, 0, 0, 0, 3, 10, 0, 0, 0,
  };
  uint8_t storage[sizeof(packet) + 1]; memcpy(storage + 1, packet, sizeof(packet));
  sfu_rtcp_member_view view = view_of(storage + 1, sizeof(packet));
  sfu_rtcp_fir fir; sfu_rtcp_fir_entry entry;
  assert(sfu_rtcp_parse_fir(&view, &fir) && fir.entry_count == 2);
  assert(sfu_rtcp_fir_entry_at(&fir, 1, &entry));
  assert(entry.target_ssrc == 3 && entry.sequence_number == 10);
}

static void test_remb_builder(void) {
  uint8_t packet[32];
  uint32_t ssrcs[] = {0x11223344u, 0xaabbccddu};
  int len = sfu_rtcp_build_remb(1, 2500000, ssrcs, 2, packet, sizeof(packet));
  assert(len == 28);
  assert(packet[0] == 0x8f && packet[1] == 206);
  assert(sfu_read_be16(packet + 2) == 6);
  assert(sfu_read_be32(packet + 4) == 1 && sfu_read_be32(packet + 8) == 0);
  assert(memcmp(packet + 12, "REMB", 4) == 0 && packet[16] == 2);
  uint32_t encoded = ((uint32_t)packet[17] << 16) | ((uint32_t)packet[18] << 8) | packet[19];
  uint8_t exponent = (uint8_t)(encoded >> 18);
  uint32_t mantissa = encoded & 0x3ffffu;
  assert(((uint64_t)mantissa << exponent) <= 2500000);
  assert(2500000 - ((uint64_t)mantissa << exponent) < (1ull << exponent));
  assert(sfu_read_be32(packet + 20) == ssrcs[0]);
  assert(sfu_read_be32(packet + 24) == ssrcs[1]);
  assert(sfu_rtcp_build_remb(1, 1000, ssrcs, 1, packet, 23) < 0);
  assert(sfu_rtcp_build_remb(1, 1000, ssrcs, 0, packet, sizeof(packet)) < 0);
}

int main(void) {
  test_pli();
  test_padding_logical_pli();
  test_fir();
  test_multiple_and_unaligned();
  test_remb_builder();
  puts("rtcp fb tests passed");
  return 0;
}
