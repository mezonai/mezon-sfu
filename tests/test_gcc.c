/* GCC estimator tests (CC-07/CC-08). Deterministic traces; no wall clock. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "congestion/gcc.h"

#define START 300000u
#define MINB 50000u
#define MAXB 5000000u

/* Feeds one packet-group boundary: `count` packets of `size` bytes in the
 * group, then a final packet 10 ms later that CLOSES the group. All times in
 * microseconds. */
static void feed_group(gcc_bwe_context_t *ctx, uint16_t *seq, int64_t *send_us, int64_t *recv_us, int count, uint32_t size, int64_t send_gap_us,
                       int64_t recv_gap_us) {
  gcc_packet_info_t p = {0};
  for (int i = 0; i < count; i++) {
    p.sequence_number = (*seq)++;
    p.send_time_us = *send_us + (i > 0 ? 0 : 0); /* same burst */
    p.receive_time_us = *recv_us + (i > 0 ? 0 : 0);
    p.size_bytes = size;
    gcc_bwe_process_twcc_packet(ctx, &p);
  }
  /* Closing packet starts the next group. */
  p.sequence_number = (*seq)++;
  p.send_time_us = *send_us + send_gap_us;
  p.receive_time_us = *recv_us + recv_gap_us;
  p.size_bytes = size;
  gcc_bwe_process_twcc_packet(ctx, &p);
  *send_us += send_gap_us;
  *recv_us += recv_gap_us;
}

/* The report's counterexample: groups sent every 10 ms, received every
 * 11 ms. Queueing delay grows 1 ms per group; the old x-axis (per-group
 * intervals) forced the regression slope to zero and never detected it.
 * The cumulative arrival-time x-axis must detect OVERUSE. */
static void test_steadily_growing_queue_detected(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  uint16_t seq = 0;
  int64_t send_us = 1000000, recv_us = 1000000;

  /* First group to establish prev_group. */
  feed_group(&ctx, &seq, &send_us, &recv_us, 1, 1200, 10000, 11000);

  gcc_bwe_usage_t saw = GCC_BWE_NORMAL;
  for (int i = 0; i < 60 && saw != GCC_BWE_OVERUSE; i++) {
    feed_group(&ctx, &seq, &send_us, &recv_us, 1, 1200, 10000, 11000);
    saw = ctx.trendline.usage_state;
  }
  assert(saw == GCC_BWE_OVERUSE);

  ctx.aimd.current_bitrate_bps = START;
  ctx.aimd.ack_bitrate_bps = MAXB;
  ctx.aimd.have_ack_bitrate = true;
  uint32_t before = ctx.aimd.current_bitrate_bps;
  feed_group(&ctx, &seq, &send_us, &recv_us, 1, 1200, 10000, 11000);
  assert(ctx.aimd.state == GCC_RATE_CTRL_DECREASE);
  assert(ctx.aimd.current_bitrate_bps < before);
}

/* Constant delay (send 10 ms, recv 10 ms) must NOT signal overuse. */
static void test_constant_delay_is_normal(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  uint16_t seq = 0;
  int64_t send_us = 1000000, recv_us = 1000000;

  feed_group(&ctx, &seq, &send_us, &recv_us, 1, 1200, 10000, 10000);
  for (int i = 0; i < 60; i++) {
    feed_group(&ctx, &seq, &send_us, &recv_us, 1, 1200, 10000, 10000);
    assert(ctx.trendline.usage_state != GCC_BWE_OVERUSE);
  }
}

/* AIMD growth is time-paced: the same number of feedback callbacks with
 * identical spacing produces identical estimates regardless of how many
 * packets each callback carried (CC-08: growth by callback count is gone). */
static void test_growth_is_time_paced(void) {
  gcc_bwe_context_t a, b;
  gcc_bwe_init(&a, START, MINB, MAXB);
  gcc_bwe_init(&b, START, MINB, MAXB);

  /* Trace A: 20 groups over 2 seconds, 1 packet per group. */
  uint16_t seq = 0;
  int64_t s = 1000000, r = 1000000;
  feed_group(&a, &seq, &s, &r, 1, 0, 100000, 100000);
  for (int i = 0; i < 20; i++) {
    feed_group(&a, &seq, &s, &r, 1, 0, 100000, 100000);
  }

  /* Trace B: same wall time and zero-byte estimator input, 5 callbacks per group. */
  seq = 0;
  s = 1000000;
  r = 1000000;
  feed_group(&b, &seq, &s, &r, 5, 0, 100000, 100000);
  for (int i = 0; i < 20; i++) {
    feed_group(&b, &seq, &s, &r, 5, 0, 100000, 100000);
  }

  assert(a.aimd.current_bitrate_bps == b.aimd.current_bitrate_bps);
  /* And it actually grew beyond start (normal path -> increase). */
  assert(a.aimd.current_bitrate_bps > START);
}

/* After an overuse-driven decrease, continued NORMAL feedback must recover
 * to INCREASE — the old machine froze in DECREASE forever (CC-08). */
static void test_no_freeze_after_overuse(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  uint16_t seq = 0;
  int64_t s = 1000000, r = 1000000;

  /* Drive into overuse. */
  feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 11000);
  for (int i = 0; i < 60 && ctx.trendline.usage_state != GCC_BWE_OVERUSE; i++) {
    feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 11000);
  }
  assert(ctx.trendline.usage_state == GCC_BWE_OVERUSE);

  /* One more group completion applies the decrease. */
  feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 11000);
  assert(ctx.aimd.state == GCC_RATE_CTRL_DECREASE);
  uint32_t after_decrease = ctx.aimd.current_bitrate_bps;
  assert(after_decrease <= START);

  /* Drain the trendline window with healthy traffic; the controller must
   * leave DECREASE (via HOLD) and return to INCREASE. */
  for (int i = 0; i < 120; i++) {
    feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 10000);
    if (ctx.aimd.state == GCC_RATE_CTRL_INCREASE) {
      break;
    }
  }
  assert(ctx.aimd.state == GCC_RATE_CTRL_INCREASE);
}

/* Continued overuse decreases repeatedly (old code decreased once). */
static void test_repeated_overuse_keeps_decreasing(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  /* Force the detector into overuse and apply it twice in a row by
   * re-driving the trendline above threshold both times. */
  uint16_t seq = 0;
  int64_t s = 1000000, r = 1000000;

  feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 20000); /* big jumps */
  uint32_t previous = UINT32_MAX;
  int decreases = 0;
  for (int i = 0; i < 200 && decreases < 2; i++) {
    feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 20000);
    if (ctx.aimd.current_bitrate_bps < previous) {
      decreases++;
      previous = ctx.aimd.current_bitrate_bps;
    }
  }
  assert(decreases >= 2);
  assert(ctx.aimd.current_bitrate_bps >= MINB); /* clamp respected */
}

/* Invalid init bounds fall back to a valid configuration. */
static void test_init_validates_bounds(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, 100, 5000000, 50000); /* min > max, start out of range */
  assert(ctx.aimd.min_bitrate_bps <= ctx.aimd.current_bitrate_bps);
  assert(ctx.aimd.current_bitrate_bps <= ctx.aimd.max_bitrate_bps);

  gcc_bwe_init(&ctx, 0, 0, 0);
  assert(ctx.aimd.min_bitrate_bps > 0);
  assert(ctx.aimd.max_bitrate_bps >= ctx.aimd.min_bitrate_bps);
}

/* A reordered packet must not rewrite a group's endpoint. */
static void test_reorder_ignored(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  gcc_packet_info_t p = {0};
  p.sequence_number = 1;
  p.send_time_us = 1000000;
  p.receive_time_us = 1000000;
  p.size_bytes = 1200;
  gcc_bwe_process_twcc_packet(&ctx, &p);
  p.sequence_number = 2;
  p.send_time_us = 1001000;
  p.receive_time_us = 1001000;
  gcc_bwe_process_twcc_packet(&ctx, &p);
  assert(ctx.current_group.last_send_time_us == 1001000);

  /* Out-of-order older packet: endpoints unchanged. */
  p.sequence_number = 3;
  p.send_time_us = 999000;
  p.receive_time_us = 1002000;
  gcc_bwe_process_twcc_packet(&ctx, &p);
  assert(ctx.current_group.last_send_time_us == 1001000);
  assert(ctx.current_group.packet_count == 2);
}

/* A long feedback gap followed by one high sample must not count the whole
 * gap as sustained overuse. */
static void test_feedback_gap_not_counted_as_overuse(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  uint16_t seq = 0;
  int64_t s = 1000000, r = 1000000;

  /* Build mild queue growth so the trend is positive but the time-over-use
   * accumulation stays below threshold. */
  feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 10500);
  for (int i = 0; i < 5; i++) {
    feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 10500);
  }
  assert(ctx.trendline.usage_state != GCC_BWE_OVERUSE);

  /* 10-second feedback gap, then one more growing sample: the gap must be
   * discarded, not accumulated. */
  s += 10000000;
  r += 10000500;
  feed_group(&ctx, &seq, &s, &r, 1, 1200, 10000, 10500);
  assert(ctx.trendline.usage_state != GCC_BWE_OVERUSE);
}

/* CC-13: loss above 10% requires 3 consecutive windows before applying 0.85x backoff;
 * moderate loss (2%-10%) blocks increases (HOLD); small loss (<=2%) is a no-op. */
static void test_loss_has_control_effect(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);
  ctx.aimd.current_bitrate_bps = 1000000;
  ctx.aimd.have_ack_bitrate = true;
  ctx.aimd.ack_bitrate_bps = 400000;

  /* Window 1: isolated heavy loss (15%) is tracked but does NOT back off immediately. */
  bool backed_off = gcc_bwe_report_loss(&ctx, 15, 100);
  assert(!backed_off);
  assert(ctx.aimd.consecutive_high_loss_windows == 1);
  assert(ctx.aimd.current_bitrate_bps == 1000000);

  /* Window 2: second high loss window. */
  backed_off = gcc_bwe_report_loss(&ctx, 15, 100);
  assert(!backed_off);
  assert(ctx.aimd.consecutive_high_loss_windows == 2);
  assert(ctx.aimd.current_bitrate_bps == 1000000);

  /* Non-high loss resets the streak. */
  backed_off = gcc_bwe_report_loss(&ctx, 1, 100);
  assert(!backed_off);
  assert(ctx.aimd.consecutive_high_loss_windows == 0);
  assert(ctx.aimd.current_bitrate_bps == 1000000);

  /* Now deliver 3 consecutive high-loss windows: 3rd window triggers 0.85x backoff. */
  assert(!gcc_bwe_report_loss(&ctx, 15, 100));
  assert(!gcc_bwe_report_loss(&ctx, 15, 100));
  backed_off = gcc_bwe_report_loss(&ctx, 15, 100);
  assert(backed_off);
  /* Basis was ack_bitrate_bps (400000) because ack < current; 400000 * 85 / 100 = 340000 */
  assert(ctx.aimd.current_bitrate_bps == 340000);
  assert(ctx.aimd.state == GCC_RATE_CTRL_HOLD);
  assert(ctx.aimd.consecutive_high_loss_windows == 0);

  /* Moderate loss (2% - 10%): holds state, no decrease below current. */
  ctx.aimd.state = GCC_RATE_CTRL_INCREASE;
  ctx.aimd.current_bitrate_bps = 500000;
  backed_off = gcc_bwe_report_loss(&ctx, 5, 100);
  assert(!backed_off);
  assert(ctx.aimd.current_bitrate_bps == 500000);
  assert(ctx.aimd.state == GCC_RATE_CTRL_HOLD);

  /* Small loss (<= 2%): no-op. */
  ctx.aimd.state = GCC_RATE_CTRL_INCREASE;
  backed_off = gcc_bwe_report_loss(&ctx, 2, 100);
  assert(!backed_off);
  assert(ctx.aimd.state == GCC_RATE_CTRL_INCREASE);

  /* Minimum clamp: repeated sustained loss clamps to min_bitrate_bps */
  for (int i = 0; i < 60; i++) {
    gcc_bwe_report_loss(&ctx, 15, 100);
  }
  assert(ctx.aimd.current_bitrate_bps == MINB);

  /* Degenerate inputs never crash or corrupt. */
  gcc_bwe_report_loss(&ctx, 0, 0);
  gcc_bwe_report_loss(&ctx, 10, 0);
  assert(ctx.aimd.current_bitrate_bps == MINB);
}

static void test_adaptive_threshold_evolution_is_gradual(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);
  assert(ctx.trendline.threshold_ms == 12.5);

  uint16_t seq = 0;
  int64_t s = 1000000, r = 1000000;
  /* Feed groups spaced by 20 ms (20000 us) with mild delay variation */
  feed_group(&ctx, &seq, &s, &r, 1, 1200, 20000, 25000);
  double init_thresh = ctx.trendline.threshold_ms;

  /* Run 5 groups (100 ms total elapsed time) */
  for (int i = 0; i < 5; i++) {
    feed_group(&ctx, &seq, &s, &r, 1, 1200, 20000, 25000);
  }

  /* In 100 ms, threshold change must be small and gradual (< 0.5 ms),
   * proving delta_s is properly in seconds and not overreacting by 1000x */
  double delta_thresh = ctx.trendline.threshold_ms - init_thresh;
  if (delta_thresh < 0) {
    delta_thresh = -delta_thresh;
  }
  assert(delta_thresh < 0.5);
  assert(ctx.trendline.threshold_ms >= 6.0 && ctx.trendline.threshold_ms <= 600.0);
}

static void test_ack_bitrate_uses_aggregate_window(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  gcc_packet_info_t p = {0};
  /* Changed to 11 bursts to reach 300ms window threshold (was 6 bursts for 150ms) */
  for (int burst = 0; burst <= 10; burst++) {
    for (int i = 0; i < 5; i++) {
      p.sequence_number++;
      p.send_time_us = 1000000 + burst * 30000;
      p.receive_time_us = 1000000 + burst * 30000;
      p.size_bytes = 1200;
      gcc_bwe_process_twcc_packet(&ctx, &p);
    }
    if (burst < 10) {
      assert(!ctx.aimd.have_ack_bitrate);
    }
  }

  assert(ctx.aimd.have_ack_bitrate);
  assert(ctx.aimd.ack_bitrate_bps > 1500000);
  assert(ctx.aimd.ack_bitrate_bps < 2500000);

  p.sequence_number++;
  p.send_time_us += 500000;
  p.receive_time_us += 500000;
  gcc_bwe_process_twcc_packet(&ctx, &p);
  assert(!ctx.aimd.have_ack_bitrate);
  assert(ctx.aimd.ack_window_bytes == p.size_bytes);
}

static void test_normal_ack_cap_does_not_decrease(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  uint16_t seq = 0;
  int64_t send_us = 1000000, recv_us = 1000000;
  feed_group(&ctx, &seq, &send_us, &recv_us, 1, 1200, 100000, 100000);

  ctx.aimd.current_bitrate_bps = 1000000;
  ctx.aimd.ack_bitrate_bps = 100000;
  ctx.aimd.have_ack_bitrate = true;
  ctx.aimd.state = GCC_RATE_CTRL_INCREASE;
  ctx.aimd.last_increase_us = recv_us - 200000;
  ctx.aimd.ack_window_bytes = 0;
  ctx.aimd.ack_window_min_recv_us = 0;
  ctx.aimd.ack_window_max_recv_us = 0;

  feed_group(&ctx, &seq, &send_us, &recv_us, 1, 1200, 100000, 100000);
  assert(ctx.trendline.usage_state == GCC_BWE_NORMAL);
  assert(ctx.aimd.current_bitrate_bps == 1000000);
}

static void test_recovery_probe_escapes_ack_rate_lock(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  uint16_t seq = 0;
  int64_t send_us = 1000000, recv_us = 1000000;
  feed_group(&ctx, &seq, &send_us, &recv_us, 1, 0, 100000, 100000);
  ctx.aimd.current_bitrate_bps = 300000;
  ctx.aimd.ack_bitrate_bps = 100000;
  ctx.aimd.have_ack_bitrate = true;
  ctx.aimd.state = GCC_RATE_CTRL_INCREASE;
  ctx.aimd.last_increase_us = recv_us - 200000;

  for (int i = 0; i < 25; i++) {
    feed_group(&ctx, &seq, &send_us, &recv_us, 1, 0, 100000, 100000);
  }
  assert(ctx.trendline.usage_state == GCC_BWE_NORMAL);
  assert(ctx.aimd.current_bitrate_bps > 300000);
  assert(ctx.aimd.current_bitrate_bps <= 350000); /* ack * 1.5 + bounded 200 kbps recovery headroom */

  assert(ctx.aimd.last_recovery_probe_us != 0);
  gcc_bwe_report_loss(&ctx, 5, 100);
  assert(ctx.aimd.last_recovery_probe_us == 0);
  assert(ctx.aimd.state == GCC_RATE_CTRL_HOLD);
}

static void test_active_probing_coordination(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  assert(!gcc_bwe_is_overusing(&ctx));
  assert(gcc_bwe_get_usage(&ctx) == GCC_BWE_NORMAL);
  assert(gcc_bwe_get_bitrate(&ctx) == START);
  assert(gcc_bwe_get_ack_bitrate(&ctx) == 0);
  assert(!gcc_bwe_has_recent_feedback(&ctx, 1000000, 500000));

  gcc_bwe_record_feedback(&ctx, 1000000);
  assert(gcc_bwe_has_recent_feedback(&ctx, 1100000, 500000));
  assert(!gcc_bwe_has_recent_feedback(&ctx, 1600000, 500000));

  /* When active probing is enabled, passive recovery probing is subordinated */
  uint16_t seq = 0;
  int64_t send_us = 1000000, recv_us = 1000000;
  feed_group(&ctx, &seq, &send_us, &recv_us, 1, 0, 100000, 100000);
  ctx.aimd.current_bitrate_bps = 300000;
  ctx.aimd.ack_bitrate_bps = 100000;
  ctx.aimd.have_ack_bitrate = true;
  ctx.aimd.state = GCC_RATE_CTRL_INCREASE;
  ctx.aimd.last_increase_us = recv_us - 200000;

  gcc_bwe_set_active_probing(&ctx, true);
  for (int i = 0; i < 25; i++) {
    feed_group(&ctx, &seq, &send_us, &recv_us, 1, 0, 100000, 100000);
  }
  /* Passive recovery probe must NOT have inflated current_bitrate_bps */
  assert(ctx.aimd.current_bitrate_bps == 300000);
  assert(ctx.aimd.last_recovery_probe_us == 0);

  gcc_bwe_set_active_probing(&ctx, false);
}

static void test_probe_cluster_completed(void) {
  gcc_bwe_context_t ctx;
  gcc_bwe_init(&ctx, START, MINB, MAXB);

  ctx.aimd.current_bitrate_bps = 300000;
  ctx.aimd.ack_bitrate_bps = 250000;
  ctx.aimd.have_ack_bitrate = true;

  /* Successful probe completed at 1.5 Mbps */
  gcc_bwe_on_probe_cluster_completed(&ctx, 1500000, 2000000);
  assert(gcc_bwe_get_bitrate(&ctx) == 1500000);
  assert(gcc_bwe_get_ack_bitrate(&ctx) == 1500000);
  assert(ctx.aimd.last_active_probe_us == 2000000);

  /* Overuse prevents probe inflation */
  ctx.trendline.usage_state = GCC_BWE_OVERUSE;
  gcc_bwe_on_probe_cluster_completed(&ctx, 2500000, 3000000);
  assert(gcc_bwe_get_bitrate(&ctx) == 1500000);
}

int main(void) {
  test_steadily_growing_queue_detected();
  test_constant_delay_is_normal();
  test_growth_is_time_paced();
  test_no_freeze_after_overuse();
  test_repeated_overuse_keeps_decreasing();
  test_init_validates_bounds();
  test_reorder_ignored();
  test_feedback_gap_not_counted_as_overuse();
  test_loss_has_control_effect();
  test_adaptive_threshold_evolution_is_gradual();
  test_ack_bitrate_uses_aggregate_window();
  test_normal_ack_cap_does_not_decrease();
  test_recovery_probe_escapes_ack_rate_lock();
  test_active_probing_coordination();
  test_probe_cluster_completed();
  printf("test_gcc: OK\n");
  return 0;
}
