#include "rtcp/rtcp_compound.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static sfu_rtcp_member_view next_item(sfu_rtcp_compound_iter *it) {
  sfu_rtcp_member_view view;
  assert(sfu_rtcp_compound_iter_next(it, &view) == SFU_RTCP_COMPOUND_ITEM);
  return view;
}

static void test_single_member(void) {
  const uint8_t rr[] = {0x80, 201, 0, 1, 0x11, 0x22, 0x33, 0x44};
  sfu_rtcp_compound_iter it;
  sfu_rtcp_compound_iter_init(&it, rr, sizeof(rr));
  sfu_rtcp_member_view view = next_item(&it);
  assert(view.member == rr && view.member_len == sizeof(rr));
  assert(view.fmt_count == 0 && view.pt == 201);
  assert(view.sender_ssrc == 0x11223344u);
  assert(view.body == rr + 8 && view.body_len == 0);
  assert(sfu_rtcp_compound_iter_next(&it, &view) == SFU_RTCP_COMPOUND_END);
}

static void test_rr_nack(void) {
  const uint8_t packet[] = {
      0x80, 201, 0, 1, 0, 0, 0, 1,
      0x81, 205, 0, 3, 0, 0, 0, 2, 0, 0, 0, 3, 0x12, 0x34, 0, 1,
  };
  sfu_rtcp_compound_iter it;
  sfu_rtcp_compound_iter_init(&it, packet, sizeof(packet));
  sfu_rtcp_member_view rr = next_item(&it);
  sfu_rtcp_member_view nack = next_item(&it);
  assert(rr.member == packet && rr.member_len == 8);
  assert(nack.member == packet + 8 && nack.member_len == 16);
  assert(nack.fmt_count == 1 && nack.pt == 205 && nack.sender_ssrc == 2);
  assert(nack.body == packet + 16 && nack.body_len == 8);
  assert(sfu_rtcp_compound_iter_next(&it, &nack) == SFU_RTCP_COMPOUND_END);
}

static void test_nack_pli_boundaries(void) {
  const uint8_t packet[] = {
      0x81, 205, 0, 3, 0, 0, 0, 4, 0, 0, 0, 5, 0xab, 0xcd, 0, 0,
      0x81, 206, 0, 2, 0, 0, 0, 6, 0, 0, 0, 7,
  };
  sfu_rtcp_compound_iter it;
  sfu_rtcp_compound_iter_init(&it, packet, sizeof(packet));
  sfu_rtcp_member_view nack = next_item(&it);
  sfu_rtcp_member_view pli = next_item(&it);
  assert(nack.member + nack.member_len == pli.member);
  assert(pli.member == packet + 16 && pli.member_len == 12);
  assert(pli.fmt_count == 1 && pli.pt == 206 && pli.body_len == 4);
  assert(sfu_rtcp_compound_iter_next(&it, &pli) == SFU_RTCP_COMPOUND_END);
}

static void expect_malformed(const uint8_t *packet, size_t len) {
  sfu_rtcp_compound_iter it;
  sfu_rtcp_member_view view;
  sfu_rtcp_compound_iter_init(&it, packet, len);
  assert(sfu_rtcp_compound_iter_next(&it, &view) == SFU_RTCP_COMPOUND_MALFORMED);
  assert(sfu_rtcp_compound_iter_next(&it, &view) == SFU_RTCP_COMPOUND_MALFORMED);
}

static void test_bad_headers_and_lengths(void) {
  const uint8_t bad_version[] = {0x40, 201, 0, 1, 0, 0, 0, 1};
  const uint8_t too_long[] = {0x80, 201, 0, 2, 0, 0, 0, 1};
  const uint8_t too_short[] = {0x80, 201, 0, 0};
  expect_malformed(bad_version, sizeof(bad_version));
  expect_malformed(too_long, sizeof(too_long));
  expect_malformed(too_short, sizeof(too_short));
}

static void test_trailing_garbage(void) {
  const uint8_t packet[] = {0x80, 201, 0, 1, 0, 0, 0, 1, 0xaa, 0xbb, 0xcc};
  sfu_rtcp_compound_iter it;
  sfu_rtcp_member_view view;
  sfu_rtcp_compound_iter_init(&it, packet, sizeof(packet));
  assert(sfu_rtcp_compound_iter_next(&it, &view) == SFU_RTCP_COMPOUND_ITEM);
  assert(sfu_rtcp_compound_iter_next(&it, &view) == SFU_RTCP_COMPOUND_MALFORMED);
}

static void test_padding(void) {
  const uint8_t valid[] = {0xa0, 201, 0, 2, 0, 0, 0, 1, 0, 0, 0, 4};
  const uint8_t zero[] = {0xa0, 201, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0};
  const uint8_t excessive[] = {0xa0, 201, 0, 2, 0, 0, 0, 1, 0, 0, 0, 9};
  sfu_rtcp_compound_iter it;
  sfu_rtcp_compound_iter_init(&it, valid, sizeof(valid));
  sfu_rtcp_member_view view = next_item(&it);
  assert(view.member_len == 8 && view.body_len == 0);
  assert(sfu_rtcp_compound_iter_next(&it, &view) == SFU_RTCP_COMPOUND_END);
  expect_malformed(zero, sizeof(zero));
  expect_malformed(excessive, sizeof(excessive));
}

static void test_max_length_no_overflow(void) {
  const uint8_t packet[] = {0x80, 201, 0xff, 0xff, 0, 0, 0, 1};
  expect_malformed(packet, sizeof(packet));
}

static void test_unaligned_buffer(void) {
  uint8_t storage[9];
  const uint8_t packet[] = {0x80, 201, 0, 1, 0xde, 0xad, 0xbe, 0xef};
  memcpy(storage + 1, packet, sizeof(packet));
  sfu_rtcp_compound_iter it;
  sfu_rtcp_compound_iter_init(&it, storage + 1, sizeof(packet));
  sfu_rtcp_member_view view = next_item(&it);
  assert(view.member == storage + 1 && view.sender_ssrc == 0xdeadbeefu);
}

int main(void) {
  test_single_member();
  test_rr_nack();
  test_nack_pli_boundaries();
  test_bad_headers_and_lengths();
  test_trailing_garbage();
  test_padding();
  test_max_length_no_overflow();
  test_unaligned_buffer();
  puts("rtcp compound tests passed");
  return 0;
}
