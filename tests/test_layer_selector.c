/* Layer selector tests (#83): hysteresis, dwell, source-switch transaction. */

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "congestion/gcc.h"
#include "runtime/scheduler.h"
#include "sfu/datadef.h"

/* UP thresholds are rung rate * 1.2: 180k, 600k, 1440k. DOWN thresholds are
 * the rung rates: 150k, 500k, 1200k. */

/* Backdates the dwell clock so the next set_bitrate is allowed to change
 * the target immediately (tests run faster than the 500 ms dwell). */
#define DWELL_EXPIRE(s_) ((s_).last_target_change_us -= 600000)

static void test_up_needs_headroom(void) {
  sfu_subscriber_scheduler_t s;
  sfu_subscriber_scheduler_init(&s, 1);

  /* Exactly at the rung rate is NOT enough to climb. */
  sfu_subscriber_scheduler_set_bitrate(&s, 1200000);
  assert(s.target_sid != 2);
  /* Clearing the 20% headroom climbs. */
  DWELL_EXPIRE(s);
  sfu_subscriber_scheduler_set_bitrate(&s, 1440000);
  assert(s.target_sid == 2 && s.target_tid == 2);
}

static void test_down_holds_at_rung_rate(void) {
  sfu_subscriber_scheduler_t s;
  sfu_subscriber_scheduler_init(&s, 1);
  sfu_subscriber_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 2);

  /* Falling between rung rate and up threshold holds the rung. */
  s.last_target_change_us -= 600000;
  sfu_subscriber_scheduler_set_bitrate(&s, 1300000);
  assert(s.target_sid == 2);

  /* Breaking the down threshold drops — but only after dwell. */
  s.last_target_change_us -= 600000;
  sfu_subscriber_scheduler_set_bitrate(&s, 1100000);
  assert(s.target_sid == 1 && s.target_tid == 2);
}

static void test_dwell_blocks_fast_flap(void) {
  sfu_subscriber_scheduler_t s;
  sfu_subscriber_scheduler_init(&s, 1);
  sfu_subscriber_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 2);

  /* Immediate change attempt within dwell: blocked. */
  sfu_subscriber_scheduler_set_bitrate(&s, 100000);
  assert(s.target_sid == 2);
}

static void test_switch_source_transaction(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sched.target_sid = 2;
  sched.target_tid = 2;
  sched.current_sid = 2;
  sched.current_tid = 2;
  sched.needs_keyframe = false;
  session.scheduler = &sched;

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 300000, 50000, 5000000);
  gcc.aimd.current_bitrate_bps = 2500000;
  session.gcc_ctx = &gcc;

  atomic_store(&session.egress_generation, 7);

  sfu_layer_selector_switch_source(&session, 42);

  assert(sched.active_publisher_id == 42);
  assert(sched.needs_keyframe == true);   /* gate armed */
  assert(sched.current_sid == 0 && sched.current_tid == 0);
  assert(atomic_load(&session.egress_generation) == 8); /* stale RTX invalidated */
  /* GCC restarts from the current estimate (2.5M, clamped within bounds) but
   * with trend/history state cleared — not the old source's trendline. */
  assert(gcc.aimd.current_bitrate_bps == 2500000);
  assert(gcc.trendline.history_count == 0);
  /* Targets survive the switch (the new path starts from the same policy). */
  assert(sched.target_sid == 2 && sched.target_tid == 2);
}

int main(void) {
  test_up_needs_headroom();
  test_down_holds_at_rung_rate();
  test_dwell_blocks_fast_flap();
  test_switch_source_transaction();
  printf("test_layer_selector: OK\n");
  return 0;
}
