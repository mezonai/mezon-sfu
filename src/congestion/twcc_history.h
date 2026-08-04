#ifndef SFU_TWCC_HISTORY_H
#define SFU_TWCC_HISTORY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "gcc.h"

// Power of 2 capacity (4096 covers ~10-15 seconds of 4K video streams)
#define SFU_TWCC_HISTORY_CAPACITY 4096
#define SFU_TWCC_HISTORY_MASK (SFU_TWCC_HISTORY_CAPACITY - 1)

typedef struct sfu_twcc_history_entry {
  uint16_t twcc_seq;
  int64_t send_time_us;
  uint32_t size_bytes;
  bool valid;
} sfu_twcc_history_entry_t;

typedef struct sfu_twcc_history {
  sfu_twcc_history_entry_t entries[SFU_TWCC_HISTORY_CAPACITY];
} sfu_twcc_history_t;

static inline void sfu_twcc_history_init(sfu_twcc_history_t *h) { memset(h, 0, sizeof(sfu_twcc_history_t)); }

// Record an outbound RTP packet's send metadata
static inline void sfu_twcc_history_record(sfu_twcc_history_t *h, uint16_t seq, int64_t send_time_us, uint32_t size_bytes) {
  uint32_t idx = seq & SFU_TWCC_HISTORY_MASK;
  h->entries[idx].twcc_seq = seq;
  h->entries[idx].send_time_us = send_time_us;
  h->entries[idx].size_bytes = size_bytes;
  h->entries[idx].valid = true;
}

// Populate out_pkt with send_time_us and size_bytes if the sequence number matches
static inline bool sfu_twcc_history_lookup(sfu_twcc_history_t *h, uint16_t seq, gcc_packet_info_t *out_pkt) {
  uint32_t idx = seq & SFU_TWCC_HISTORY_MASK;
  sfu_twcc_history_entry_t *entry = &h->entries[idx];

  // Validate entry existence and sequence number match (guards against ring wrap-around)
  if (entry->valid && entry->twcc_seq == seq) {
    out_pkt->send_time_us = entry->send_time_us;
    out_pkt->size_bytes = entry->size_bytes;
    return true;
  }
  return false;
}

#endif  // SFU_TWCC_HISTORY_H
