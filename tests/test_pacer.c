#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "congestion/pacer.h"

#define KB 1000u

static void test_inactive_admits_everything(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  int64_t now = 1000000;
  /* No set_rate call: transport-cc not negotiated -> unpaced. */
  for (int i = 0; i < 1000; i++) {
    assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_ENH, 1200, &now));
  }
  assert(p.dropped_enh == 0);
  /* Inactive pacer never rewrites the caller's timestamp. */
  assert(now == 1000000);
}

static void test_zero_rate_disables(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  sfu_pacer_set_rate(&p, 500 * KB, 1000000);
  assert(p.active);
  sfu_pacer_set_rate(&p, 0, 2000000);
  assert(!p.active);
  int64_t now = 2000000;
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_ENH, 1200, &now));
}

static void test_burst_then_throttle(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  /* 1 Mbps estimate -> pacing 2.5 Mbps. Bucket cap = 2.5Mbps/8 * 40ms =
   * 12500 bytes. */
  sfu_pacer_set_rate(&p, 1000 * KB, 1000000);
  assert(p.active);
  assert(p.bucket_cap_bytes == 12500);

  /* A fresh bucket admits a 12 KB I-frame at once... */
  int64_t now = 1000000;
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));
  /* ...but the next base-layer packet borrows almost the whole window... */
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));
  assert(p.balance_bytes < 0);
  /* ...and an enhancement packet that would exceed a full burst window of
   * debt is dropped instead of queued. */
  assert(p.balance_bytes == 12500 - 24000);
  assert(!sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_ENH, 12000, &now));
  assert(p.dropped_enh == 1);
}

static void test_audio_never_dropped_under_debt(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  sfu_pacer_set_rate(&p, 1000 * KB, 1000000);
  int64_t now = 1000000;

  /* Drive deep into debt with video. */
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));
  assert(p.balance_bytes < 0);

  /* Audio and RTX always borrow through: only enhancement video drops. */
  for (int i = 0; i < 10; i++) {
    assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_AUDIO, 200, &now));
    assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_RTX, 1000, &now));
  }
  assert(p.sent[SFU_PACER_CLASS_AUDIO] == 10);
  assert(p.sent[SFU_PACER_CLASS_RTX] == 10);
}

static void test_refill_restores_budget(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  sfu_pacer_set_rate(&p, 1000 * KB, 1000000);
  int64_t now = 1000000;
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));

  /* 40 ms later the bucket has refilled exactly one burst window: pacing
   * 2.5 Mbps = 312500 B/s, over 40 ms = 12500 bytes, clamped at the cap.
   * Debt -11500 + 12500 = +1000. */
  now += 40000;
  assert(sfu_pacer_debt_after(&p, 1000, now) == 0);
  assert(sfu_pacer_debt_after(&p, 2000, now) == 1000);

  /* Enhancement fits again inside the window. */
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_ENH, 1000, &now));
}

static void test_retune_preserves_debt(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  sfu_pacer_set_rate(&p, 1000 * KB, 1000000);
  int64_t now = 1000000;
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 12000, &now));
  int64_t debt = p.balance_bytes;
  assert(debt < 0);

  /* A new estimate retunes the rate but must NOT wipe the debt — otherwise
   * every TWCC feedback would grant a free burst. */
  sfu_pacer_set_rate(&p, 2000 * KB, now);
  assert(p.balance_bytes == debt);
  assert(p.pacing_bps == 2000 * KB * 5 / 2);
}

static void test_admission_timestamp_is_send_time(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  sfu_pacer_set_rate(&p, 1000 * KB, 123456789);
  int64_t now = 123456789 + 5000; /* clock advanced since arming */
  assert(sfu_pacer_should_send(&p, SFU_PACER_CLASS_VIDEO_BASE, 100, &now));
  /* CC-14: the caller records exactly this value as the TWCC send time. */
  assert(now == 123456789 + 5000);
}

/* CC-16: the RTX budget serves a burst of loss immediately, then throttles
 * sustained line-rate NACKs to a fraction of the pacing rate, and refills
 * over time. */
static void test_rtx_budget_window(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  /* 4 Mbps -> pacing 10 Mbps -> RTX budget 2.5 Mbps; cap = 2500000/8 *
   * 40ms = 12500 bytes (above the 4096 floor, so exact arithmetic). */
  sfu_pacer_set_rate(&p, 4000 * KB, 1000000);
  assert(p.rtx_budget_cap_bytes == 12500);

  int64_t now = 1000000;
  /* Burst: the full window is available at once: floor(12500/1200) = 10. */
  int served = 0;
  while (sfu_pacer_rtx_allow(&p, 1200, now)) {
    served++;
    assert(served < 100); /* must terminate: budget is finite */
  }
  assert(served == 10);
  assert(p.rtx_dropped_budget == 1); /* the failing call counted */

  /* Sustained: 10 ms later 2500000/8 * 10ms = 3125 bytes refill the 500
   * remainder -> 3625: exactly 3 more packets (3600). */
  now += 10000;
  assert(sfu_pacer_rtx_allow(&p, 1200, now));
  assert(sfu_pacer_rtx_allow(&p, 1200, now));
  assert(sfu_pacer_rtx_allow(&p, 1200, now));
  assert(!sfu_pacer_rtx_allow(&p, 1200, now)); /* 25 bytes left */
}

static void test_rtx_budget_unpaced_when_inactive(void) {
  sfu_pacer_t p;
  sfu_pacer_init(&p);
  int64_t now = 1000000;
  for (int i = 0; i < 100; i++) {
    assert(sfu_pacer_rtx_allow(&p, 1200, now));
  }
  assert(p.rtx_dropped_budget == 0);
}

int main(void) {
  test_inactive_admits_everything();
  test_zero_rate_disables();
  test_burst_then_throttle();
  test_audio_never_dropped_under_debt();
  test_refill_restores_budget();
  test_retune_preserves_debt();
  test_admission_timestamp_is_send_time();
  test_rtx_budget_window();
  test_rtx_budget_unpaced_when_inactive();
  printf("test_pacer: OK\n");
  return 0;
}
