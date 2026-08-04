/* Deterministic mutation sweep over the wire parsers (#86 fuzz-lite).
 *
 * Seeds valid RTCP compound / TWCC / NACK / RTP-extension / VP9 payloads,
 * then flips every byte through a small value set and truncates at every
 * length, asserting the parsers never crash, never read out of bounds
 * (ASan/UBSan witness that), and always terminate. This is the poor-man's
 * fuzz harness: the same driver shape can later call into libFuzzer's
 * LLVMFuzzerTestOneInput without changing the parsers.
 *
 * Run under ASan/UBSan/TSan in CI for real value; a plain run only checks
 * termination and return-contract sanity. */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "congestion/twcc_parser.h"
#include "media/svc/vp9_parser.h"
#include "rtcp/rtcp_compound.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtx.h"
#include "sfu/datadef.h"

#define BUF 2048

/* A valid TWCC feedback member: base seq 100, 8 status chunks (run of
 * received-large), reference time, recv deltas. */
static size_t seed_twcc(uint8_t *b) {
  size_t n = 0;
  b[n++] = 0x00; b[n++] = 0x64;             /* base seq = 100 */
  b[n++] = 0x00; b[n++] = 0x08;             /* status count = 8 */
  b[n++] = 0x12; b[n++] = 0x34; b[n++] = 0x56; /* reference time 24-bit */
  b[n++] = 0x00;                            /* fb pkt count */
  b[n++] = 0x9F; b[n++] = 0xFF;             /* run chunk: large delta x8 (symbol 01111111 -> 0x9F 0xFF?) */
  for (int i = 0; i < 8; i++) { b[n++] = 0x10; b[n++] = 0x00; } /* large deltas */
  b[n++] = 0x00; b[n++] = 0x00;             /* padding */
  return n;
}

static size_t seed_nack(uint8_t *b) {
  size_t n = 0;
  b[n++] = 0xDE; b[n++] = 0xAD; b[n++] = 0xBE; b[n++] = 0xEF; /* media ssrc */
  b[n++] = 0x00; b[n++] = 0x2A;             /* pid 42 */
  b[n++] = 0xFF; b[n++] = 0xFF;             /* blp */
  return n;
}

static size_t seed_rtcp(uint8_t *b) {
  size_t n = 0;
  b[n++] = 0x81; b[n++] = 205;              /* V=2, FMT=1, RTPFB */
  b[n++] = 0x00; b[n++] = 0x03;             /* length: 4 words - 1 */
  b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x01; /* sender ssrc */
  b[n++] = 0xDE; b[n++] = 0xAD; b[n++] = 0xBE; b[n++] = 0xEF; /* media ssrc */
  b[n++] = 0x00; b[n++] = 0x2A; b[n++] = 0x00; b[n++] = 0x01; /* one FCI */
  return n;
}

static size_t seed_rtp_ext(uint8_t *b) {
  size_t n = 0;
  b[n++] = 0x90;                            /* V=2, X=1 */
  b[n++] = 96;                              /* PT */
  b[n++] = 0x00; b[n++] = 0x01;             /* seq */
  b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x10; b[n++] = 0x00; /* ts */
  b[n++] = 0xAA; b[n++] = 0xBB; b[n++] = 0xCC; b[n++] = 0xDD; /* ssrc */
  b[n++] = 0xBE; b[n++] = 0xDE;             /* one-byte ext profile */
  b[n++] = 0x00; b[n++] = 0x00;             /* ext length 0 words */
  return n;
}

static size_t seed_vp9(uint8_t *b) {
  /* I=1,P=0,L=0,F=0 | B E | picture id 7-bit | ... minimal flexible */
  size_t n = 0;
  b[n++] = 0x80;                            /* I=1, P=0 */
  b[n++] = 0x06;                            /* B=0,E=1 ... actually 0x06 sets B,E? keep arbitrary-but-valid-ish */
  b[n++] = 0x2A;                            /* picture id 42 */
  b[n++] = 0x00;                            /* TL0PICIDX if L; harmless extra */
  return n;
}

static const uint8_t k_values[] = {0x00, 0x01, 0x7F, 0x80, 0xFF};

static void sweep(const char *name, const uint8_t *seed, size_t seed_len, void (*exercise)(const uint8_t *, size_t)) {
  uint8_t buf[BUF];
  /* Truncation sweep. */
  for (size_t len = 0; len <= seed_len; len++) {
    exercise(seed, len);
  }
  /* Byte-flip sweep. */
  for (size_t i = 0; i < seed_len; i++) {
    for (size_t v = 0; v < sizeof(k_values); v++) {
      memcpy(buf, seed, seed_len);
      buf[i] ^= k_values[v];
      exercise(buf, seed_len);
    }
  }
  (void)name;
}

static void exercise_twcc(const uint8_t *d, size_t len) {
  sfu_twcc_parser_t p;
  if (sfu_twcc_parser_init(&p, d, len, 0) != 0) {
    return;
  }
  gcc_packet_info_t item;
  size_t guard = 0;
  while (sfu_twcc_parser_next(&p, &item)) {
    assert(++guard < 65536); /* must terminate */
  }
}

static void exercise_nack(const uint8_t *d, size_t len) {
  sfu_nack_parser_t p;
  if (!sfu_nack_parser_init(&p, d, len)) {
    return;
  }
  uint16_t seq;
  size_t guard = 0;
  while (sfu_nack_parser_next(&p, &seq)) {
    assert(++guard < 65536);
  }
}

static void exercise_rtcp(const uint8_t *d, size_t len) {
  sfu_rtcp_compound_iter it;
  sfu_rtcp_compound_iter_init(&it, d, len);
  sfu_rtcp_member_view view;
  size_t guard = 0;
  for (;;) {
    sfu_rtcp_compound_result rc = sfu_rtcp_compound_iter_next(&it, &view);
    if (rc != SFU_RTCP_COMPOUND_ITEM) {
      break;
    }
    assert(++guard < 4096);
  }
}

static void exercise_rtp_ext(const uint8_t *d, size_t len) {
  if (len > BUF / 2) {
    return;
  }
  uint8_t buf[BUF];
  memcpy(buf, d, len);
  size_t io_len = len;
  /* Any ext id 1..14; capacity is the full buffer so growth is in-bounds. */
  (void)sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), 5, 0x1234, &io_len);
}

static void exercise_vp9(const uint8_t *d, size_t len) {
  sfu_vp9_descriptor_t desc;
  (void)sfu_parse_vp9_descriptor(d, len, &desc);
}

int main(void) {
  uint8_t seed[BUF];

  size_t n = seed_twcc(seed);
  sweep("twcc", seed, n, exercise_twcc);

  n = seed_nack(seed);
  sweep("nack", seed, n, exercise_nack);

  n = seed_rtcp(seed);
  sweep("rtcp", seed, n, exercise_rtcp);

  n = seed_rtp_ext(seed);
  sweep("rtp_ext", seed, n, exercise_rtp_ext);

  n = seed_vp9(seed);
  sweep("vp9", seed, n, exercise_vp9);

  printf("test_parser_fuzz: OK\n");
  return 0;
}
