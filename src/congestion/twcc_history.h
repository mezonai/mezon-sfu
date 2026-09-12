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
  bool received_consumed;
  bool loss_reported;
  bool is_probe;
  uint32_t probe_cluster_id;
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
  h->entries[idx].received_consumed = false;
  h->entries[idx].loss_reported = false;
  h->entries[idx].is_probe = false;
  h->entries[idx].probe_cluster_id = 0;
}

// Record an outbound probe RTP packet's send metadata with cluster ID
static inline void sfu_twcc_history_record_probe(sfu_twcc_history_t *h, uint16_t seq, int64_t send_time_us, uint32_t size_bytes, uint32_t cluster_id) {
  uint32_t idx = seq & SFU_TWCC_HISTORY_MASK;
  h->entries[idx].twcc_seq = seq;
  h->entries[idx].send_time_us = send_time_us;
  h->entries[idx].size_bytes = size_bytes;
  h->entries[idx].valid = true;
  h->entries[idx].received_consumed = false;
  h->entries[idx].loss_reported = false;
  h->entries[idx].is_probe = true;
  h->entries[idx].probe_cluster_id = cluster_id;
}

// Populate out_pkt with send_time_us and size_bytes if the sequence number matches, along with probe metadata
static inline bool sfu_twcc_history_lookup_ext(sfu_twcc_history_t *h, uint16_t seq, gcc_packet_info_t *out_pkt,
                                               bool *is_probe, uint32_t *probe_cluster_id) {
  uint32_t idx = seq & SFU_TWCC_HISTORY_MASK;
  sfu_twcc_history_entry_t *entry = &h->entries[idx];

  // Validate entry existence and sequence number match (guards against ring wrap-around)
  if (entry->valid && entry->twcc_seq == seq) {
    if (out_pkt) {
      out_pkt->send_time_us = entry->send_time_us;
      out_pkt->size_bytes = entry->size_bytes;
    }
    if (is_probe) {
      *is_probe = entry->is_probe;
    }
    if (probe_cluster_id) {
      *probe_cluster_id = entry->probe_cluster_id;
    }
    return true;
  }
  return false;
}

// Populate out_pkt with send_time_us and size_bytes if the sequence number matches
static inline bool sfu_twcc_history_lookup(sfu_twcc_history_t *h, uint16_t seq, gcc_packet_info_t *out_pkt) {
  return sfu_twcc_history_lookup_ext(h, seq, out_pkt, NULL, NULL);
}

static inline bool sfu_twcc_history_consume_received_ext(sfu_twcc_history_t *h, uint16_t seq, gcc_packet_info_t *out_pkt,
                                                         bool *was_loss_reported, bool *is_probe, uint32_t *probe_cluster_id) {
  uint32_t idx = seq & SFU_TWCC_HISTORY_MASK;
  sfu_twcc_history_entry_t *entry = &h->entries[idx];
  if (!entry->valid || entry->twcc_seq != seq || entry->received_consumed) {
    return false;
  }
  if (out_pkt) {
    out_pkt->send_time_us = entry->send_time_us;
    out_pkt->size_bytes = entry->size_bytes;
  }
  if (was_loss_reported) {
    *was_loss_reported = entry->loss_reported;
  }
  if (is_probe) {
    *is_probe = entry->is_probe;
  }
  if (probe_cluster_id) {
    *probe_cluster_id = entry->probe_cluster_id;
  }
  entry->received_consumed = true;
  return true;
}

static inline bool sfu_twcc_history_consume_received(sfu_twcc_history_t *h, uint16_t seq, gcc_packet_info_t *out_pkt, bool *was_loss_reported) {
  return sfu_twcc_history_consume_received_ext(h, seq, out_pkt, was_loss_reported, NULL, NULL);
}

static inline bool sfu_twcc_history_report_loss_once(sfu_twcc_history_t *h, uint16_t seq) {
  uint32_t idx = seq & SFU_TWCC_HISTORY_MASK;
  sfu_twcc_history_entry_t *entry = &h->entries[idx];
  if (!entry->valid || entry->twcc_seq != seq || entry->received_consumed || entry->loss_reported) {
    return false;
  }
  entry->loss_reported = true;
  return true;
}

#endif  // SFU_TWCC_HISTORY_H
