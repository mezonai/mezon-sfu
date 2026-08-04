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
  uint32_t packets_lost;
} sfu_twcc_parser_t;

int sfu_twcc_parser_init(sfu_twcc_parser_t *parser, const uint8_t *data, size_t len, int64_t unwrap_anchor_us);
bool sfu_twcc_parser_next(sfu_twcc_parser_t *parser, gcc_packet_info_t *out_pkt);

#endif  // SFU_TWCC_PARSER_H
