#ifndef SFU_TWCC_FEEDBACK_H
#define SFU_TWCC_FEEDBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_TWCC_RECV_CAPACITY 1024
#define SFU_TWCC_RECV_MASK (SFU_TWCC_RECV_CAPACITY - 1)

#define SFU_TWCC_FEEDBACK_INTERVAL_US 50000LL

#define SFU_TWCC_FEEDBACK_MAX_PACKETS 64

typedef struct sfu_twcc_recv_entry {
  uint16_t seq;
  int64_t arrival_us;
  bool valid;
} sfu_twcc_recv_entry_t;

typedef struct sfu_twcc_recv_tracker {
  sfu_twcc_recv_entry_t entries[SFU_TWCC_RECV_CAPACITY];
  bool started;
  uint16_t report_start_seq;
  uint16_t latest_seq;
  int64_t last_feedback_us;
  uint8_t fb_pkt_count;
} sfu_twcc_recv_tracker_t;

void sfu_twcc_recv_tracker_init(sfu_twcc_recv_tracker_t *t);
void sfu_twcc_recv_tracker_record(sfu_twcc_recv_tracker_t *t, uint16_t seq, int64_t arrival_us);
bool sfu_twcc_recv_tracker_pending(const sfu_twcc_recv_tracker_t *t);
int sfu_twcc_feedback_build(sfu_twcc_recv_tracker_t *t, uint32_t sender_ssrc, uint32_t media_ssrc, int64_t now_us, uint8_t *buf, size_t cap);

#endif
