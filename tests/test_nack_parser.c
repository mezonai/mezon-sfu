#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtp/rtx.h"
#include "util/netbytes.h"

/* Build a NACK member: V=2, FMT=1, PT=205, sender SSRC, media SSRC, then
 * pid/blp FCI entries. len_word is total 32-bit words minus one. */
static size_t build_nack(uint8_t *buf, uint32_t media_ssrc, const uint16_t *fci, size_t fci_words) {
  size_t fci_bytes = fci_words * 2;
  size_t total = 12 + fci_bytes;
  buf[0] = 0x80 | 1; /* V=2, FMT=1 */
  buf[1] = 205;
  sfu_write_be16(buf + 2, (uint16_t)(total / 4 - 1));
  sfu_write_be32(buf + 4, 0xdeadbeefu);
  sfu_write_be32(buf + 8, media_ssrc);
  for (size_t i = 0; i < fci_words; i++) {
    sfu_write_be16(buf + 12 + i * 2, fci[i]);
  }
  return total;
}

static void test_valid_single_block(void) {
  uint8_t buf[64];
  uint16_t fci[] = {100, 0x0005}; /* pid=100, blp bits 0 and 2 -> 101, 103 */
  size_t len = build_nack(buf, 0x11223344u, fci, 2);

  sfu_nack_parser_t p;
  assert(sfu_nack_parser_init(&p, buf, len));
  assert(sfu_nack_parser_media_ssrc(&p) == 0x11223344u);

  uint16_t seq = 0;
  assert(sfu_nack_parser_next(&p, &seq) && seq == 100);
  assert(sfu_nack_parser_next(&p, &seq) && seq == 101);
  assert(sfu_nack_parser_next(&p, &seq) && seq == 103);
  assert(!sfu_nack_parser_next(&p, &seq));
}

static void test_valid_multi_block(void) {
  uint8_t buf[64];
  uint16_t fci[] = {50, 0x0001, 200, 0x0000}; /* 50, 51 ; 200 */
  size_t len = build_nack(buf, 7u, fci, 4);

  sfu_nack_parser_t p;
  assert(sfu_nack_parser_init(&p, buf, len));

  uint16_t seq = 0;
  assert(sfu_nack_parser_next(&p, &seq) && seq == 50);
  assert(sfu_nack_parser_next(&p, &seq) && seq == 51);
  assert(sfu_nack_parser_next(&p, &seq) && seq == 200);
  assert(!sfu_nack_parser_next(&p, &seq));
}

static void test_exact_member_bounds(void) {
  /* Parser must stop at the member end even if a live buffer continues. */
  uint8_t storage[64];
  uint16_t fci[] = {300, 0x0000};
  size_t len = build_nack(storage, 1u, fci, 2);
  memset(storage + len, 0xff, sizeof(storage) - len);

  sfu_nack_parser_t p;
  assert(sfu_nack_parser_init(&p, storage, len));
  uint16_t seq = 0;
  assert(sfu_nack_parser_next(&p, &seq) && seq == 300);
  assert(!sfu_nack_parser_next(&p, &seq));
}

static void test_rejects(void) {
  uint8_t buf[64];
  uint16_t fci[] = {100, 0x0000};
  size_t len = build_nack(buf, 1u, fci, 2);
  sfu_nack_parser_t p;

  /* Wrong PT (206 = PSFB). */
  uint8_t saved_pt = buf[1];
  buf[1] = 206;
  assert(!sfu_nack_parser_init(&p, buf, len));
  buf[1] = saved_pt;

  /* Wrong FMT (15 = TWCC, not generic NACK). */
  uint8_t saved_b0 = buf[0];
  buf[0] = 0x80 | 15;
  assert(!sfu_nack_parser_init(&p, buf, len));
  buf[0] = saved_b0;

  /* Wrong RTP version. */
  buf[0] = 0x40 | 1;
  assert(!sfu_nack_parser_init(&p, buf, len));
  buf[0] = saved_b0;

  /* Too short: header + SSRCs but no FCI block. */
  assert(!sfu_nack_parser_init(&p, buf, 12));
  assert(!sfu_nack_parser_init(&p, buf, 8));
  assert(!sfu_nack_parser_init(&p, NULL, len));

  /* Failed init leaves the parser inert. */
  assert(!sfu_nack_parser_next(&p, &(uint16_t){0}));
}

static void test_fci_multiple_of_four(void) {
  /* Member with an FCI of 6 bytes (2 mod 4) is rejected even though the
   * declared RTCP length word covers it. */
  uint8_t buf[64];
  uint16_t fci[] = {100, 0x0000, 200};
  size_t len = build_nack(buf, 1u, fci, 3); /* 12 + 6 = 18 bytes */
  sfu_nack_parser_t p;
  assert(!sfu_nack_parser_init(&p, buf, len));

  /* FCI of exactly 4 bytes is the minimal valid member. */
  uint16_t fci_ok[] = {100, 0x0000};
  len = build_nack(buf, 1u, fci_ok, 2);
  assert(sfu_nack_parser_init(&p, buf, len));
  uint16_t seq = 0;
  assert(sfu_nack_parser_next(&p, &seq) && seq == 100);
  assert(!sfu_nack_parser_next(&p, &seq));
}

int main(void) {
  test_valid_single_block();
  test_valid_multi_block();
  test_exact_member_bounds();
  test_rejects();
  test_fci_multiple_of_four();
  printf("test_nack_parser: OK\n");
  return 0;
}
