#include "congestion/twcc_feedback.h"

#include <string.h>

#include "util/log.h"
#include "util/netbytes.h"

#define TWCC_FIXED_LEN 20u

#define TWCC_STATUS_NOT_RECEIVED 0
#define TWCC_STATUS_SMALL_DELTA 1
#define TWCC_STATUS_LARGE_DELTA 2

#define TWCC_REF_UNIT_US 64000LL
#define TWCC_SMALL_DELTA_UNIT_US 250LL
#define TWCC_SMALL_DELTA_MAX_US (255LL * TWCC_SMALL_DELTA_UNIT_US)

void sfu_twcc_recv_tracker_init(sfu_twcc_recv_tracker_t *t) { memset(t, 0, sizeof(*t)); }

void sfu_twcc_recv_tracker_record(sfu_twcc_recv_tracker_t *t, uint16_t seq, int64_t arrival_us) {
  uint32_t idx = seq & SFU_TWCC_RECV_MASK;
  t->entries[idx].seq = seq;
  t->entries[idx].arrival_us = arrival_us;
  t->entries[idx].valid = true;

  if (!t->started) {
    t->started = true;
    t->report_start_seq = seq;
    t->latest_seq = seq;
    return;
  }

  int32_t fwd = (int32_t)(uint16_t)(seq - t->latest_seq);
  if (fwd > 0 && fwd < 0x8000) {
    t->latest_seq = seq;
  }
}

bool sfu_twcc_recv_tracker_pending(const sfu_twcc_recv_tracker_t *t) {
  if (!t->started) {
    return false;
  }
  int32_t fwd = (int32_t)(uint16_t)(t->latest_seq - t->report_start_seq);
  return fwd >= 0 && fwd < 0x8000;
}

static void write_run_chunk(uint8_t **p, uint8_t symbol, uint16_t run) {
  uint16_t chunk = (uint16_t)(((uint16_t)symbol << 13) | (run & 0x1FFF));
  sfu_write_be16(*p, chunk);
  *p += 2;
}

int sfu_twcc_feedback_build(sfu_twcc_recv_tracker_t *t, uint32_t sender_ssrc, uint32_t media_ssrc, int64_t now_us, uint8_t *buf, size_t cap) {
  if (!t || !buf || !sfu_twcc_recv_tracker_pending(t)) {
    return 0;
  }

  uint8_t status[SFU_TWCC_FEEDBACK_MAX_PACKETS];
  int64_t arrival[SFU_TWCC_FEEDBACK_MAX_PACKETS];
  uint16_t base_seq = t->report_start_seq;
  uint32_t window = 0;
  uint32_t received_count = 0;
  for (;;) {
    base_seq = t->report_start_seq;
    window = (uint32_t)(uint16_t)(t->latest_seq - base_seq) + 1u;
    if (window > SFU_TWCC_FEEDBACK_MAX_PACKETS) {
      window = SFU_TWCC_FEEDBACK_MAX_PACKETS;
    }
    if (window == 0) {
      return 0;
    }

    received_count = 0;
    for (uint32_t i = 0; i < window; i++) {
      uint16_t seq = (uint16_t)(base_seq + i);
      sfu_twcc_recv_entry_t *e = &t->entries[seq & SFU_TWCC_RECV_MASK];
      if (e->valid && e->seq == seq) {
        arrival[i] = e->arrival_us;
        status[i] = TWCC_STATUS_SMALL_DELTA;
        received_count++;
      } else {
        arrival[i] = 0;
        status[i] = TWCC_STATUS_NOT_RECEIVED;
      }
    }
    if (received_count > 0) {
      break;
    }
    t->report_start_seq = (uint16_t)(base_seq + window);
    if (!sfu_twcc_recv_tracker_pending(t)) {
      return 0;
    }
  }

  int64_t ref_us = 0;
  for (uint32_t i = 0; i < window; i++) {
    if (status[i] != TWCC_STATUS_NOT_RECEIVED) {
      ref_us = arrival[i];
      break;
    }
  }
  int64_t ref_floored_us = (ref_us / TWCC_REF_UNIT_US) * TWCC_REF_UNIT_US;
  int64_t ref_quant = (ref_us / TWCC_REF_UNIT_US) & 0xFFFFFF;

  int32_t delta_units[SFU_TWCC_FEEDBACK_MAX_PACKETS];
  {
    int64_t clock_us = ref_floored_us;
    for (uint32_t i = 0; i < window; i++) {
      if (status[i] == TWCC_STATUS_NOT_RECEIVED) {
        delta_units[i] = 0;
        continue;
      }
      int64_t d_us = arrival[i] - clock_us;
      int64_t units = (d_us >= 0) ? (d_us / TWCC_SMALL_DELTA_UNIT_US) : -((-d_us + TWCC_SMALL_DELTA_UNIT_US - 1) / TWCC_SMALL_DELTA_UNIT_US);
      if (units < 0 || units > 255) {
        status[i] = TWCC_STATUS_LARGE_DELTA;
        if (units > 32767) {
          units = 32767;
        }
        if (units < -32768) {
          units = -32768;
        }
      }
      delta_units[i] = (int32_t)units;
      clock_us = arrival[i];
    }
  }

  uint8_t chunk_buf[SFU_TWCC_FEEDBACK_MAX_PACKETS * 2];
  uint8_t *cp = chunk_buf;
  {
    uint32_t i = 0;
    while (i < window) {
      uint8_t sym = status[i];
      uint32_t run = 1;
      while (i + run < window && status[i + run] == sym && run < 0x1FFF) {
        run++;
      }
      write_run_chunk(&cp, sym, (uint16_t)run);
      i += run;
    }
  }
  size_t chunk_len = (size_t)(cp - chunk_buf);

  uint8_t delta_buf[SFU_TWCC_FEEDBACK_MAX_PACKETS * 2];
  uint8_t *dp = delta_buf;
  for (uint32_t i = 0; i < window; i++) {
    if (status[i] == TWCC_STATUS_SMALL_DELTA) {
      *dp++ = (uint8_t)delta_units[i];
    } else if (status[i] == TWCC_STATUS_LARGE_DELTA) {
      sfu_write_be16(dp, (uint16_t)(int16_t)delta_units[i]);
      dp += 2;
    }
  }
  size_t delta_len = (size_t)(dp - delta_buf);

  size_t total = TWCC_FIXED_LEN + chunk_len + delta_len;
  size_t pad = (4u - (total & 3u)) & 3u;
  size_t packet_len = total + pad;
  if (packet_len > cap) {
    return -1;
  }

  buf[0] = 0x80 | 15;
  buf[1] = 205;
  uint32_t words = (uint32_t)(packet_len / 4u) - 1u;
  sfu_write_be16(buf + 2, (uint16_t)words);
  sfu_write_be32(buf + 4, sender_ssrc);
  sfu_write_be32(buf + 8, media_ssrc);
  sfu_write_be16(buf + 12, base_seq);
  sfu_write_be16(buf + 14, (uint16_t)window);
  buf[16] = (uint8_t)((ref_quant >> 16) & 0xFF);
  buf[17] = (uint8_t)((ref_quant >> 8) & 0xFF);
  buf[18] = (uint8_t)(ref_quant & 0xFF);
  buf[19] = t->fb_pkt_count++;

  memcpy(buf + TWCC_FIXED_LEN, chunk_buf, chunk_len);
  memcpy(buf + TWCC_FIXED_LEN + chunk_len, delta_buf, delta_len);
  if (pad) {
    memset(buf + total, 0, pad);
  }

  t->report_start_seq = (uint16_t)(base_seq + window);
  t->last_feedback_us = now_us;

  {
    int64_t min_d = 0, max_d = 0;
    for (uint32_t i = 0; i < window; i++) {
      if (status[i] == TWCC_STATUS_NOT_RECEIVED) {
        continue;
      }
      int64_t d = delta_units[i] * TWCC_SMALL_DELTA_UNIT_US;
      if (d < min_d) {
        min_d = d;
      }
      if (d > max_d) {
        max_d = d;
      }
    }
    SFU_LOG_INFO("twcc_fb: base=%u win=%u recv=%u lost=%u dmin=%lldus dmax=%lldus", base_seq, window, received_count, window - received_count,
                 (long long)min_d, (long long)max_d);
  }

  return (int)packet_len;
}
