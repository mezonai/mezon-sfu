/* TWCC feedback parser tests (CC-06/CC-12). */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "congestion/twcc_feedback.h"
#include "congestion/twcc_parser.h"
#include "util/netbytes.h"

/* Builds a TWCC feedback member (no RTCP padding) into buf. chunks/deltas
 * are appended raw after the 20-byte fixed header. */
static size_t build_fb(uint8_t *buf, uint16_t base_seq, uint16_t status_count, uint32_t ref_time_24b, uint8_t fb_count, const uint8_t *chunks,
                       size_t chunks_len, const uint8_t *deltas, size_t deltas_len) {
  buf[0] = 0x80 | 15; /* V=2, FMT=15 */
  buf[1] = 205;
  size_t total = 20 + chunks_len + deltas_len;
  sfu_write_be16(buf + 2, (uint16_t)(total / 4 - 1));
  sfu_write_be32(buf + 4, 1);         /* sender SSRC */
  sfu_write_be32(buf + 8, 0x0a0b0c0d); /* media SSRC */
  sfu_write_be16(buf + 12, base_seq);
  sfu_write_be16(buf + 14, status_count);
  buf[16] = (uint8_t)(ref_time_24b >> 16);
  buf[17] = (uint8_t)(ref_time_24b >> 8);
  buf[18] = (uint8_t)ref_time_24b;
  buf[19] = fb_count;
  memcpy(buf + 20, chunks, chunks_len);
  memcpy(buf + 20 + chunks_len, deltas, deltas_len);
  return total;
}

/* Run-length chunk: T=0 | symbol<<13 | run. */
static uint16_t run_chunk(uint8_t symbol, uint16_t run) { return (uint16_t)(symbol << 13) | (run & 0x1FFF); }

/* A single small-delta run: reference time + N*250us steps are preserved
 * exactly (CC-12: no truncation to integer ms). */
static void test_small_deltas_microsecond_precision(void) {
  uint8_t buf[64];
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, run_chunk(TWCC_STATUS_SMALL_DELTA, 3));
  uint8_t deltas[3] = {1, 2, 3}; /* 250, 500, 750 us */

  size_t len = build_fb(buf, 100, 3, 1000, 0, chunk_bytes, 2, deltas, 3);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) == 0);

  gcc_packet_info_t pkt;
  int64_t base_us = 1000LL * 64000;
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(pkt.sequence_number == 100);
  assert(pkt.receive_time_us == base_us + 250);
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(pkt.receive_time_us == base_us + 250 + 500);
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(pkt.receive_time_us == base_us + 250 + 500 + 750);
  assert(!sfu_twcc_parser_next(&p, &pkt));
}

/* Large (signed) deltas apply negative offsets. */
static void test_large_delta_signed(void) {
  uint8_t buf[64];
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, run_chunk(TWCC_STATUS_LARGE_DELTA, 2));
  uint8_t deltas[4];
  sfu_write_be16(deltas, (uint16_t)40);      /* +10000 us */
  sfu_write_be16(deltas + 2, (uint16_t)-40); /* -10000 us */

  size_t len = build_fb(buf, 7, 2, 500, 0, chunk_bytes, 2, deltas, 4);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) == 0);

  gcc_packet_info_t pkt;
  int64_t base_us = 500LL * 64000;
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(pkt.receive_time_us == base_us + 10000);
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(pkt.receive_time_us == base_us);
  assert(!sfu_twcc_parser_next(&p, &pkt));
}

/* Not-received statuses consume no delta and are skipped. */
static void test_not_received_skipped(void) {
  uint8_t buf[64];
  /* 1-bit vector, T=1 S=0: statuses [1,0,1] -> received, lost, received. */
  uint16_t vec = 0x8000 | (1u << 13) | (1u << 11);
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, vec);
  uint8_t deltas[2] = {4, 4}; /* 1000 us each */

  size_t len = build_fb(buf, 10, 3, 100, 0, chunk_bytes, 2, deltas, 2);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) == 0);

  gcc_packet_info_t pkt;
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(pkt.sequence_number == 10);
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(pkt.sequence_number == 12); /* 11 was lost, skipped */
  assert(!sfu_twcc_parser_next(&p, &pkt));
}

/* CC-06: zero-length run must fail init (previously underflowed the 16-bit
 * counter to 65535 and fabricated statuses). */
static void test_zero_run_rejected(void) {
  uint8_t buf[64];
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, run_chunk(TWCC_STATUS_SMALL_DELTA, 0));
  size_t len = build_fb(buf, 1, 1, 100, 0, chunk_bytes, 2, NULL, 0);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) != 0);
}

/* CC-06: reserved symbol 3 in a run chunk fails init. */
static void test_reserved_run_symbol_rejected(void) {
  uint8_t buf[64];
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, run_chunk(TWCC_STATUS_RESERVED, 5));
  size_t len = build_fb(buf, 1, 5, 100, 0, chunk_bytes, 2, NULL, 0);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) != 0);
}

/* CC-06: reserved symbol inside a two-bit vector aborts the batch (the
 * parser returns false rather than desynchronizing deltas). */
static void test_reserved_vector_symbol_aborts(void) {
  uint8_t buf[64];
  /* Two-bit vector: statuses [SMALL, RESERVED, ...] -> 01 11 00... */
  uint16_t vec = 0x8000 | 0x4000 | (1u << 12) | (3u << 10);
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, vec);
  uint8_t deltas[1] = {1};

  size_t len = build_fb(buf, 1, 7, 100, 0, chunk_bytes, 2, deltas, 1);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) == 0);

  gcc_packet_info_t pkt;
  assert(sfu_twcc_parser_next(&p, &pkt)); /* first status is a valid small delta */
  assert(!sfu_twcc_parser_next(&p, &pkt)); /* reserved aborts */
}

/* CC-06: chunks covering fewer than packet_status_count statuses fail init. */
static void test_truncated_chunk_run_rejected(void) {
  uint8_t buf[64];
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, run_chunk(TWCC_STATUS_SMALL_DELTA, 2)); /* only 2 of 5 */
  size_t len = build_fb(buf, 1, 5, 100, 0, chunk_bytes, 2, NULL, 0);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) != 0);
}

/* Truncated delta section: iteration aborts instead of reading OOB. */
static void test_truncated_delta_aborts(void) {
  uint8_t buf[64];
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, run_chunk(TWCC_STATUS_LARGE_DELTA, 2));
  uint8_t deltas[2]; /* only one 16-bit delta for two LARGE statuses */
  sfu_write_be16(deltas, 10);

  size_t len = build_fb(buf, 1, 2, 100, 0, chunk_bytes, 2, deltas, 2);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) == 0);

  gcc_packet_info_t pkt;
  assert(sfu_twcc_parser_next(&p, &pkt));
  assert(!sfu_twcc_parser_next(&p, &pkt));
}

/* CC-12: the 24-bit reference time unwraps against the anchor instead of
 * jumping back ~12.4 days. */
static void test_reference_time_unwrap(void) {
  const int64_t range_us = (1LL << 24) * 64000LL;

  uint8_t buf[64];
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, run_chunk(TWCC_STATUS_SMALL_DELTA, 1));
  uint8_t deltas[1] = {0};

  /* Reference near the wrap point: 2^24 - 1 ticks. */
  size_t len = build_fb(buf, 1, 1, 0xFFFFFF, 0, chunk_bytes, 2, deltas, 1);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) == 0);
  gcc_packet_info_t pkt;
  assert(sfu_twcc_parser_next(&p, &pkt));
  int64_t first_ref = pkt.receive_time_us;
  assert(first_ref == range_us - 64000);

  /* Next feedback wraps to a small reference; anchored at the previous
   * value, it must unwrap FORWARD, not backward. */
  len = build_fb(buf, 2, 1, 5, 0, chunk_bytes, 2, deltas, 1);
  assert(sfu_twcc_parser_init(&p, buf, len, first_ref) == 0);
  assert(sfu_twcc_parser_next(&p, &pkt));
  int64_t expected = first_ref + ((5LL * 64000 + range_us - first_ref % range_us) % range_us);
  assert(pkt.receive_time_us == expected);
  assert(pkt.receive_time_us > first_ref);
}

/* Excess statuses in the final vector chunk are accepted and ignored. */
static void test_final_chunk_excess_tolerated(void) {
  uint8_t buf[64];
  /* 1-bit vector covers 14 statuses; only 3 are reported. */
  uint16_t vec = 0x8000 | (1u << 13) | (1u << 12) | (1u << 11);
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, vec);
  uint8_t deltas[3] = {1, 1, 1};

  size_t len = build_fb(buf, 50, 3, 100, 0, chunk_bytes, 2, deltas, 3);
  sfu_twcc_parser_t p;
  assert(sfu_twcc_parser_init(&p, buf, len, 0) == 0);

  gcc_packet_info_t pkt;
  int n = 0;
  while (sfu_twcc_parser_next(&p, &pkt)) {
    n++;
  }
  assert(n == 3);
  assert(p.packets_processed == 3);
}

static void test_status_iterator_includes_losses(void) {
  uint8_t buf[64];
  uint16_t vec = 0x8000 | (1u << 13) | (1u << 11);
  uint8_t chunk_bytes[2];
  sfu_write_be16(chunk_bytes, vec);
  uint8_t deltas[2] = {4, 8};

  size_t len = build_fb(buf, 20, 3, 100, 0, chunk_bytes, 2, deltas, 2);
  sfu_twcc_parser_t parser;
  assert(sfu_twcc_parser_init(&parser, buf, len, 0) == 0);

  sfu_twcc_status_t status;
  assert(sfu_twcc_parser_next_status(&parser, &status));
  assert(status.sequence_number == 20);
  assert(status.status == TWCC_STATUS_SMALL_DELTA);
  assert(sfu_twcc_parser_next_status(&parser, &status));
  assert(status.sequence_number == 21);
  assert(status.status == TWCC_STATUS_NOT_RECEIVED);
  assert(status.receive_time_us == 0);
  assert(sfu_twcc_parser_next_status(&parser, &status));
  assert(status.sequence_number == 22);
  assert(status.status == TWCC_STATUS_SMALL_DELTA);
  assert(!sfu_twcc_parser_next_status(&parser, &status));
  assert(!parser.failed);
}

static void test_feedback_builder_mixed_deltas(void) {
  sfu_twcc_recv_tracker_t tracker;
  uint8_t buf[256];
  sfu_twcc_recv_tracker_init(&tracker);
  sfu_twcc_recv_tracker_record(&tracker, 10, 1000100);
  sfu_twcc_recv_tracker_record(&tracker, 11, 1070130);
  sfu_twcc_recv_tracker_record(&tracker, 12, 1071240);

  int len = sfu_twcc_feedback_build(&tracker, 1, 0x12345678, 1100000, buf, sizeof(buf));
  assert(len > 0);
  assert((buf[0] & 0x20u) != 0);
  assert(sfu_read_be32(buf + 8) == 0x12345678);

  uint16_t chunk = sfu_read_be16(buf + 20);
  assert((chunk & 0xc000u) == 0xc000u);
  assert(((chunk >> 12) & 3u) == TWCC_STATUS_SMALL_DELTA);
  assert(((chunk >> 10) & 3u) == TWCC_STATUS_LARGE_DELTA);
  assert(((chunk >> 8) & 3u) == TWCC_STATUS_SMALL_DELTA);

  sfu_twcc_parser_t parser;
  assert(sfu_twcc_parser_init(&parser, buf, (size_t)len, 0) == 0);
  gcc_packet_info_t pkt;
  assert(sfu_twcc_parser_next(&parser, &pkt));
  assert(pkt.sequence_number == 10);
  assert(pkt.receive_time_us == 1000000);
  assert(sfu_twcc_parser_next(&parser, &pkt));
  assert(pkt.sequence_number == 11);
  assert(pkt.receive_time_us == 1070250);
  assert(sfu_twcc_parser_next(&parser, &pkt));
  assert(pkt.sequence_number == 12);
  assert(pkt.receive_time_us == 1071250);
  assert(!sfu_twcc_parser_next(&parser, &pkt));
}

int main(void) {
  test_small_deltas_microsecond_precision();
  test_large_delta_signed();
  test_not_received_skipped();
  test_zero_run_rejected();
  test_reserved_run_symbol_rejected();
  test_reserved_vector_symbol_aborts();
  test_truncated_chunk_run_rejected();
  test_truncated_delta_aborts();
  test_reference_time_unwrap();
  test_final_chunk_excess_tolerated();
  test_status_iterator_includes_losses();
  test_feedback_builder_mixed_deltas();
  printf("test_twcc_parser: OK\n");
  return 0;
}
