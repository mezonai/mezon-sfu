#include "twcc_parser.h"
#include "util/netbytes.h"

int sfu_twcc_parser_init(sfu_twcc_parser_t *parser, const uint8_t *data, size_t len) {
  // Basic RTCP Header (4) + Sender SSRC (4) + Media SSRC (4) + TWCC Header (8) = 20 bytes min
  if (len < 20) {
    return -1;
  }

  parser->data = data;
  parser->len = len;

  // TWCC specific fields start at offset 12 (after Sender SSRC and Media SSRC)
  parser->base_seq = sfu_read_be16(data + 12);
  parser->packet_status_count = sfu_read_be16(data + 14);

  uint32_t reference_time_24b = sfu_read_be24(data + 16);
  // Reference time is in multiples of 64ms
  parser->current_time_ms = (int64_t)reference_time_24b * 64;

  // Feedback packet count is at data[19], but we don't strictly need it for parsing deltas

  parser->current_seq = parser->base_seq;
  parser->packets_processed = 0;

  parser->chunk_offset = 20;  // First chunk starts here
  parser->statuses_left_in_chunk = 0;

  // We need to scan the chunks to find where the receive deltas begin
  size_t temp_offset = 20;
  uint16_t temp_processed = 0;

  while (temp_processed < parser->packet_status_count && temp_offset + 2 <= len) {
    uint16_t chunk = sfu_read_be16(data + temp_offset);
    temp_offset += 2;

    if ((chunk & 0x8000) == 0) {
      // Run length chunk
      temp_processed += (chunk & 0x1FFF);
    } else {
      // Status vector chunk
      bool is_two_bit = (chunk & 0x4000) != 0;
      temp_processed += is_two_bit ? 7 : 14;
    }
  }

  parser->delta_offset = temp_offset;
  return 0;  // Success
}

// Helper to read the next status symbol from the current chunk
static uint8_t get_next_status(sfu_twcc_parser_t *parser) {
  if (parser->statuses_left_in_chunk == 0) {
    if (parser->chunk_offset + 2 > parser->len) {
      return TWCC_STATUS_NOT_RECEIVED;  // EOF
    }

    parser->current_chunk = sfu_read_be16(parser->data + parser->chunk_offset);
    parser->chunk_offset += 2;

    if ((parser->current_chunk & 0x8000) == 0) {
      parser->is_run_length = true;
      parser->run_length_symbol = (parser->current_chunk >> 13) & 0x03;
      parser->statuses_left_in_chunk = parser->current_chunk & 0x1FFF;
    } else {
      parser->is_run_length = false;
      // bit 14 indicates symbol size (0 = 1-bit, 1 = 2-bit)
      parser->statuses_left_in_chunk = (parser->current_chunk & 0x4000) ? 7 : 14;
    }
  }

  parser->statuses_left_in_chunk--;

  if (parser->is_run_length) {
    return parser->run_length_symbol;
  } else {
    bool is_two_bit = (parser->current_chunk & 0x4000) != 0;
    uint8_t status = 0;

    if (is_two_bit) {
      int shift = parser->statuses_left_in_chunk * 2;
      status = (parser->current_chunk >> shift) & 0x03;
    } else {
      int shift = parser->statuses_left_in_chunk;
      status = (parser->current_chunk >> shift) & 0x01;
      // 1-bit vector maps 0 -> NOT_RECEIVED, 1 -> SMALL_DELTA
    }
    return status;
  }
}

bool sfu_twcc_parser_next(sfu_twcc_parser_t *parser, gcc_packet_info_t *out_pkt) {
  while (parser->packets_processed < parser->packet_status_count) {
    uint8_t status = get_next_status(parser);
    uint16_t seq = parser->current_seq++;
    parser->packets_processed++;

    if (status == TWCC_STATUS_NOT_RECEIVED) {
      continue;  // Skip lost packets, they don't have receive deltas
    }

    int64_t recv_delta_ms = 0;

    if (status == TWCC_STATUS_SMALL_DELTA) {
      if (parser->delta_offset + 1 > parser->len) {
        return false;  // Corrupted
      }
      uint8_t delta_250us = parser->data[parser->delta_offset++];
      recv_delta_ms = (delta_250us * 250) / 1000;
    } else if (status == TWCC_STATUS_LARGE_DELTA) {
      if (parser->delta_offset + 2 > parser->len) {
        return false;  // Corrupted
      }
      int16_t delta_250us = (int16_t)sfu_read_be16(parser->data + parser->delta_offset);
      parser->delta_offset += 2;
      recv_delta_ms = (delta_250us * 250) / 1000;
    }

    parser->current_time_ms += recv_delta_ms;

    out_pkt->sequence_number = seq;
    out_pkt->receive_time_ms = parser->current_time_ms;

    // Note: TWCC feedback does NOT contain send times or sizes!
    // You MUST look these up from your local sending history buffer
    // using the parsed `out_pkt->sequence_number`.
    out_pkt->send_time_ms = 0;
    out_pkt->size_bytes = 0;

    return true;
  }

  return false;  // Reached end of statuses
}
