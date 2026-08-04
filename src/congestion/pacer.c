#include "congestion/pacer.h"

#include <string.h>

/* Pacing factor: send up to 2.5x the GCC estimate so a legitimate I-frame
 * burst is not serialized over seconds, while sustained above-estimate
 * sending still drains the bucket and throttles. Matches libwebrtc's
 * default pacing factor. */
#define SFU_PACER_FACTOR_NUM 5
#define SFU_PACER_FACTOR_DEN 2

/* Bucket cap: 40 ms worth of tokens at the pacing rate — the classic
 * pacer "max debt"/burst window. Small enough to bound added queueing
 * delay, large enough to absorb one video frame. */
#define SFU_PACER_BURST_US 40000LL

/* Floor under the computed cap so tiny estimates (or the startup ramp)
 * still allow a single MTU-sized packet per burst. */
#define SFU_PACER_MIN_BUCKET_BYTES 4096LL

/* Enhancement-layer packets may be dropped when admitting them would push
 * the bucket further into debt than one burst window: they are the
 * lowest-value bytes and the decoder recovers via layer up-switch or a
 * keyframe. Base classes (audio/RTX/video base) are never dropped by the
 * pacer — they borrow into debt instead, and the debt throttles whatever
 * follows. This is the "stale drop" policy: an enhancement packet this
 * late is worth less than the queue delay it would add to audio. */
static bool sfu_pacer_class_droppable(sfu_pacer_class_t cls) { return cls == SFU_PACER_CLASS_VIDEO_ENH; }

void sfu_pacer_init(sfu_pacer_t *p) { memset(p, 0, sizeof(*p)); }

void sfu_pacer_set_rate(sfu_pacer_t *p, uint32_t bps, int64_t now_us) {
  if (bps == 0) {
    p->active = false;
    p->pacing_bps = 0;
    return;
  }
  uint64_t paced = (uint64_t)bps * SFU_PACER_FACTOR_NUM / SFU_PACER_FACTOR_DEN;
  p->pacing_bps = (uint32_t)paced;
  int64_t cap = (int64_t)paced / 8 * SFU_PACER_BURST_US / 1000000LL;
  if (cap < SFU_PACER_MIN_BUCKET_BYTES) {
    cap = SFU_PACER_MIN_BUCKET_BYTES;
  }
  p->bucket_cap_bytes = cap;
  p->rtx_budget_bps = (uint32_t)(paced / 4);
  int64_t rtx_cap = (int64_t)p->rtx_budget_bps / 8 * SFU_PACER_BURST_US / 1000000LL;
  if (rtx_cap < SFU_PACER_MIN_BUCKET_BYTES) {
    rtx_cap = SFU_PACER_MIN_BUCKET_BYTES;
  }
  p->rtx_budget_cap_bytes = rtx_cap;
  if (!p->active) {
    /* Fresh start: begin with a full burst window and start the clock now. */
    p->balance_bytes = p->bucket_cap_bytes;
    p->last_refill_us = now_us;
    p->rtx_budget_bytes = p->rtx_budget_cap_bytes;
    p->rtx_last_refill_us = now_us;
    p->active = true;
  }
}

static void sfu_pacer_refill(sfu_pacer_t *p, int64_t now_us) {
  if (p->last_refill_us == 0 || now_us <= p->last_refill_us) {
    if (p->last_refill_us == 0) {
      p->last_refill_us = now_us;
    }
    return;
  }
  int64_t elapsed_us = now_us - p->last_refill_us;
  p->last_refill_us = now_us;
  /* bytes = bps/8 * us/1e6, computed in int64: pacing_bps <= ~1e10 and
   * elapsed is bounded by the dwell between calls, so no overflow. */
  int64_t tokens = (int64_t)p->pacing_bps / 8 * elapsed_us / 1000000LL;
  if (tokens <= 0) {
    return;
  }
  p->balance_bytes += tokens;
  if (p->balance_bytes > p->bucket_cap_bytes) {
    p->balance_bytes = p->bucket_cap_bytes;
  }
}

int64_t sfu_pacer_debt_after(const sfu_pacer_t *p, uint32_t bytes, int64_t now_us) {
  if (!p->active) {
    return 0;
  }
  /* Peek without mutating: replicate the refill arithmetic on a copy. */
  sfu_pacer_t tmp = *p;
  sfu_pacer_refill(&tmp, now_us);
  int64_t after = tmp.balance_bytes - (int64_t)bytes;
  return after < 0 ? -after : 0;
}

bool sfu_pacer_should_send(sfu_pacer_t *p, sfu_pacer_class_t cls, uint32_t bytes, int64_t *inout_now_us) {
  if (!p->active) {
    return true;
  }
  int64_t now_us = *inout_now_us;
  sfu_pacer_refill(p, now_us);

  int64_t after = p->balance_bytes - (int64_t)bytes;
  if (after < 0 && sfu_pacer_class_droppable(cls) && -after > p->bucket_cap_bytes) {
    p->dropped_enh++;
    return false;
  }

  p->balance_bytes = after;
  p->sent[cls]++;
  *inout_now_us = now_us; /* admission boundary == recorded send time (CC-14) */
  return true;
}

bool sfu_pacer_rtx_allow(sfu_pacer_t *p, uint32_t bytes, int64_t now_us) {
  if (!p->active) {
    /* No estimate, no pacing: keep the pre-budget behavior (serve every
     * deduped, capped NACK). */
    return true;
  }
  if (p->rtx_last_refill_us == 0) {
    p->rtx_last_refill_us = now_us;
  } else if (now_us > p->rtx_last_refill_us) {
    int64_t elapsed_us = now_us - p->rtx_last_refill_us;
    p->rtx_last_refill_us = now_us;
    int64_t tokens = (int64_t)p->rtx_budget_bps / 8 * elapsed_us / 1000000LL;
    if (tokens > 0) {
      p->rtx_budget_bytes += tokens;
      if (p->rtx_budget_bytes > p->rtx_budget_cap_bytes) {
        p->rtx_budget_bytes = p->rtx_budget_cap_bytes;
      }
    }
  }

  if ((int64_t)bytes > p->rtx_budget_bytes) {
    p->rtx_dropped_budget++;
    return false;
  }
  p->rtx_budget_bytes -= (int64_t)bytes;
  return true;
}
