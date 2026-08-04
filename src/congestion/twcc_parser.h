#ifndef SFU_TWCC_PARSER_H
#define SFU_TWCC_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "congestion/gcc.h"

// TWCC Status Symbols
#define TWCC_STATUS_NOT_RECEIVED 0
#define TWCC_STATUS_SMALL_DELTA 1
#define TWCC_STATUS_LARGE_DELTA 2
#define TWCC_STATUS_RESERVED 3

/* Transport-wide CC feedback parser (CC-06/CC-12).
 *
 * Timing is carried in integer MICROSECONDS end to end: the reference time
 * (64 ms units) and the small/large deltas (250 µs units) are converted once
 * at parse time, so no 250/500/750 µs quantum is ever truncated to zero.
 * gcc_packet_info_t time fields are microseconds; the GCC estimator consumes
 * them with matching units.
 *
 * The parser is strictly validating: malformed chunk structure (zero-length
 * runs, reserved symbols, run/vector counts exceeding packet_status_count)
 * fails init, and delta truncation fails iteration rather than fabricating
 * statuses. sfu_twcc_parser_next returns one received packet per call; lost
 * packets (NOT_RECEIVED) are skipped internally.
 *
 * The 24-bit reference time wraps every ~12.4 days. Callers that parse a
 * feedback series should pass the previous reference time (in microseconds)
 * as unwrap_anchor so the new reference is unwrapped to the nearest epoch;
 * pass the parsed reference verbatim for the first feedback. */

typedef struct {
  const uint8_t *data;
  size_t len;

  uint16_t base_seq;
  uint16_t packet_status_count;
  int64_t current_time_us;
  uint16_t current_seq;

  // Chunk reading state
  size_t chunk_offset;
  size_t delta_offset;

  uint16_t current_chunk;
  bool is_run_length;
  uint8_t run_length_symbol;
  uint16_t statuses_left_in_chunk;

  uint32_t packets_processed;
  /* Statuses seen so far with NOT_RECEIVED (CC-13): callers use this to give
   * loss a control effect instead of dropping it on the floor. */
  uint32_t packets_lost;
} sfu_twcc_parser_t;

int sfu_twcc_parser_init(sfu_twcc_parser_t *parser, const uint8_t *data, size_t len, int64_t unwrap_anchor_us);
bool sfu_twcc_parser_next(sfu_twcc_parser_t *parser, gcc_packet_info_t *out_pkt);

#endif  // SFU_TWCC_PARSER_H
