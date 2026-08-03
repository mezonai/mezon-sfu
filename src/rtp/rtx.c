#include "rtx.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include "sfu/datadef.h"
#include "util/alloc.h"

void sfu_nack_parser_init(sfu_nack_parser_t *parser, const uint8_t *data, uint32_t len) {
  // RTCP Header (4) + Sender SSRC (4) + Media SSRC (4) = 12 bytes
  if (len < 16) {  // Must have at least one 4-byte NACK block
    parser->nack_ptr = NULL;
    return;
  }
  parser->nack_ptr = data + 12;
  parser->nack_end = data + len;
  parser->bit_index = 16;  // Force immediate read of the first block
}

bool sfu_nack_parser_next(sfu_nack_parser_t *parser, uint16_t *lost_seq) {
  if (!parser->nack_ptr) {
    return false;
  }

  while (parser->nack_ptr < parser->nack_end) {
    // If we exhausted the previous 16-bit mask, load the next NACK block
    if (parser->bit_index > 15) {
      parser->current_pid = ntohs(*(uint16_t *)(parser->nack_ptr));
      parser->current_blp = ntohs(*(uint16_t *)(parser->nack_ptr + 2));
      parser->nack_ptr += 4;
      parser->bit_index = -1;  // -1 signals to return the base PID itself
    }

    // Yield the base Packet ID first
    if (parser->bit_index == -1) {
      *lost_seq = parser->current_pid;
      parser->bit_index++;
      return true;
    }

    // Iterate through the 16-bit BLP mask to find additional lost packets
    while (parser->bit_index < 16) {
      bool is_lost = (parser->current_blp & (1 << parser->bit_index)) != 0;
      int offset = parser->bit_index + 1;
      parser->bit_index++;

      if (is_lost) {
        *lost_seq = parser->current_pid + offset;
        return true;
      }
    }
  }

  return false;  // No more lost packets in this RTCP message
}

// Initialize the cache and pre-allocate packet buffers.
// Returns 0 on success, -1 if any entry buffer allocation fails (cache left
// cleaned so the caller can free the cache struct itself).
int sfu_rtx_cache_init(sfu_rtx_cache_t *cache) {
  memset(cache, 0, sizeof(sfu_rtx_cache_t));
  cache->next_rtx_seq = 0;

  // Pre-allocate memory for the packet copies to avoid malloc() in the hot path.
  // 1024 entries * SFU_MAX_PAYLOAD_SIZE bytes per subscriber.
  for (int i = 0; i < SFU_RTX_CACHE_SIZE; i++) {
    cache->entries[i].data = (uint8_t *)SFU_CALLOC(1, SFU_MAX_PAYLOAD_SIZE);
    if (!cache->entries[i].data) {
      // Free already-allocated buffers and leave the cache zeroed/invalid.
      for (int j = 0; j < i; j++) {
        SFU_FREE(cache->entries[j].data);
        cache->entries[j].data = NULL;
      }
      return -1;
    }
    cache->entries[i].valid = false;
  }
  return 0;
}

void sfu_rtx_cache_put(sfu_rtx_cache_t *cache, uint16_t seq, const uint8_t *data, uint32_t len, uint32_t rtx_ssrc, uint8_t rtx_pt) {
  // RTX rebuild inserts a 2-byte OSN before the original payload, so the
  // cached packet must leave room for that expansion. SRTP overhead is
  // bounded separately by the caller's protect call, which already receives
  // the destination buffer capacity (cap).
  if (len + 2 > SFU_MAX_PAYLOAD_SIZE) {
    return;
  }
  uint32_t idx = seq & SFU_RTX_CACHE_MASK;
  sfu_rtx_entry_t *entry = &cache->entries[idx];
  entry->seq = seq;
  entry->len = len;
  entry->rtx_ssrc = rtx_ssrc;
  entry->rtx_pt = rtx_pt;
  memcpy(entry->data, data, len);
  entry->valid = true;
}

bool sfu_rtx_cache_get(sfu_rtx_cache_t *cache, uint16_t seq, uint8_t *out_data, uint32_t *out_len, uint32_t *out_rtx_ssrc, uint8_t *out_rtx_pt) {
  uint32_t idx = seq & SFU_RTX_CACHE_MASK;
  sfu_rtx_entry_t *entry = &cache->entries[idx];
  if (entry->valid && entry->seq == seq) {
    memcpy(out_data, entry->data, entry->len);
    *out_len = entry->len;
    *out_rtx_ssrc = entry->rtx_ssrc;
    *out_rtx_pt = entry->rtx_pt;
    return true;
  }
  return false;
}

// Cleanup function to free pre-allocated buffers when session closes
void sfu_rtx_cache_destroy(sfu_rtx_cache_t *cache) {
  if (!cache) {
    return;
  }
  for (int i = 0; i < SFU_RTX_CACHE_SIZE; i++) {
    if (cache->entries[i].data) {
      SFU_FREE(cache->entries[i].data);
      cache->entries[i].data = NULL;
    }
  }
}
