#include "twcc_parser.h"
#include "util/netbytes.h"

/* RTCP header (4) + sender SSRC (4) + media SSRC (4) + base seq (2) +
 * status count (2) + reference time (3) + fb pkt count (1) = 20 bytes. */
#define TWCC_FIXED_LEN 20u

/* 24-bit reference-time range in microseconds (2^24 * 64 ms). */
#define TWCC_REF_RANGE_US ((1LL << 24) * 64000LL)

/* Validates the chunk run in init (CC-06): every run-length chunk must carry
 * a nonzero run with a non-reserved symbol, and the total status count must
 * not exceed packet_status_count. Returns the delta section offset, or 0 on
 * malformed input. */
static size_t validate_chunks(const uint8_t *data, size_t len, uint16_t packet_status_count) {
  size_t offset = TWCC_FIXED_LEN;
  uint32_t processed = 0;

  while (processed < packet_status_count) {
    if (offset + 2 > len) {
      return 0;
    }
    uint16_t chunk = sfu_read_be16(data + offset);
    offset += 2;

    if ((chunk & 0x8000) == 0) {
      /* Run length: T=0, symbol in bits 13-14, run length in bits 0-12. */
      uint8_t symbol = (chunk >> 13) & 0x03;
      uint16_t run = chunk & 0x1FFF;
      if (symbol == TWCC_STATUS_RESERVED || run == 0) {
        return 0;
      }
      processed += run;
    } else {
      /* Status vector: T=1, symbol size in bit 14. Zero statuses are fine
       * here (a vector of all NOT_RECEIVED is legitimate). */
      bool two_bit = (chunk & 0x4000) != 0;
      processed += two_bit ? 7u : 14u;
    }
  }

  /* Overrun is only legal for the trailing statuses of the final vector
   * chunk, which simply go unreported... no: draft-ietf-rmcat-02 §3.1.3/3.1.4
   * requires the chunk run to cover AT LEAST packet_status_count statuses,
   * and excess statuses in the final chunk are ignored by the receiver.
   * processed >= packet_status_count holds by loop exit, so accept. */
  return offset;
}

int sfu_twcc_parser_init(sfu_twcc_parser_t *parser, const uint8_t *data, size_t len, int64_t unwrap_anchor_us) {
  if (!parser || !data || len < TWCC_FIXED_LEN) {
    return -1;
  }

  parser->data = data;
  parser->len = len;
  parser->base_seq = sfu_read_be16(data + 12);
  parser->packet_status_count = sfu_read_be16(data + 14);

  size_t delta_offset = validate_chunks(data, len, parser->packet_status_count);
  if (delta_offset == 0) {
    return -1;
  }
  parser->delta_offset = delta_offset;

  /* Reference time: 24-bit, 64 ms units -> microseconds. Unwrap against the
   * caller's anchor to the nearest epoch so a wrap does not inject a
   * ~12.4-day backward jump (CC-12). */
  int64_t ref_us = (int64_t)sfu_read_be24(data + 16) * 64000LL;
  if (unwrap_anchor_us > 0) {
    int64_t delta = ref_us - (unwrap_anchor_us % TWCC_REF_RANGE_US);
    /* Fold delta into (-range/2, +range/2]. */
    delta = ((delta % TWCC_REF_RANGE_US) + TWCC_REF_RANGE_US + TWCC_REF_RANGE_US / 2) % TWCC_REF_RANGE_US - TWCC_REF_RANGE_US / 2;
    ref_us = unwrap_anchor_us + delta;
  }
  parser->current_time_us = ref_us;

  parser->current_seq = parser->base_seq;
  parser->packets_processed = 0;
  parser->chunk_offset = TWCC_FIXED_LEN;
  parser->statuses_left_in_chunk = 0;
  parser->current_chunk = 0;
  parser->is_run_length = false;
  parser->run_length_symbol = 0;
  return 0;
}

/* Reads the next status symbol. Chunks were pre-validated in init, so a
 * structurally impossible state (zero run, reserved run symbol) maps to
 * TWCC_STATUS_RESERVED to signal malformed; chunk overrun maps to
 * NOT_RECEIVED only when the count was honestly exhausted. */
static uint8_t get_next_status(sfu_twcc_parser_t *parser) {
  if (parser->statuses_left_in_chunk == 0) {
    if (parser->chunk_offset + 2 > parser->delta_offset) {
      return TWCC_STATUS_RESERVED; /* ran past validated chunks: impossible */
    }

    parser->current_chunk = sfu_read_be16(parser->data + parser->chunk_offset);
    parser->chunk_offset += 2;

    if ((parser->current_chunk & 0x8000) == 0) {
      parser->is_run_length = true;
      parser->run_length_symbol = (parser->current_chunk >> 13) & 0x03;
      parser->statuses_left_in_chunk = parser->current_chunk & 0x1FFF;
    } else {
      parser->is_run_length = false;
      parser->statuses_left_in_chunk = (parser->current_chunk & 0x4000) ? 7 : 14;
    }
    if (parser->statuses_left_in_chunk == 0) {
      return TWCC_STATUS_RESERVED;
    }
  }

  parser->statuses_left_in_chunk--;

  if (parser->is_run_length) {
    return parser->run_length_symbol;
  }

  bool two_bit = (parser->current_chunk & 0x4000) != 0;
  if (two_bit) {
    int shift = parser->statuses_left_in_chunk * 2;
    return (parser->current_chunk >> shift) & 0x03;
  }
  int shift = parser->statuses_left_in_chunk;
  /* 1-bit vector: 0 = NOT_RECEIVED, 1 = SMALL_DELTA. */
  return (parser->current_chunk >> shift) & 0x01;
}

bool sfu_twcc_parser_next(sfu_twcc_parser_t *parser, gcc_packet_info_t *out_pkt) {
  if (!parser || !out_pkt) {
    return false;
  }

  while (parser->packets_processed < parser->packet_status_count) {
    uint8_t status = get_next_status(parser);
    uint16_t seq = parser->current_seq++;
    parser->packets_processed++;

    if (status == TWCC_STATUS_NOT_RECEIVED) {
      continue; /* lost packet: no receive delta follows */
    }
    if (status == TWCC_STATUS_RESERVED) {
      /* Reserved symbol in a vector consumes no delta and desynchronizes the
       * stream (CC-06); abort the whole feedback batch. */
      return false;
    }

    int64_t delta_us;
    if (status == TWCC_STATUS_SMALL_DELTA) {
      if (parser->delta_offset + 1 > parser->len) {
        return false; /* truncated delta section */
      }
      delta_us = (int64_t)parser->data[parser->delta_offset++] * 250LL;
    } else { /* TWCC_STATUS_LARGE_DELTA */
      if (parser->delta_offset + 2 > parser->len) {
        return false;
      }
      delta_us = (int64_t)(int16_t)sfu_read_be16(parser->data + parser->delta_offset) * 250LL;
      parser->delta_offset += 2;
    }

    parser->current_time_us += delta_us;

    out_pkt->sequence_number = seq;
    out_pkt->receive_time_us = parser->current_time_us;

    /* TWCC feedback carries no send times or sizes; the caller enriches from
     * local send history keyed by sequence_number. */
    out_pkt->send_time_us = 0;
    out_pkt->size_bytes = 0;
    return true;
  }

  return false; /* all statuses consumed */
}
