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
  static sfu_session_scheduler_slot_t slots[SFU_SESSION_SCHEDULER_CAP];
  memset(slots, 0, sizeof(slots));
  session.schedulers = slots;
  /* Pre-existing per-publisher state for source 1 (the source being left). */
  sfu_subscriber_scheduler_t *old = sfu_session_scheduler_for(&session, 1);
  assert(old != NULL);
  old->target_sid = 2;
  old->target_tid = 2;
  old->current_sid = 2;
  old->current_tid = 2;
  old->needs_keyframe = false;

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 300000, 50000, 5000000);
  gcc.aimd.current_bitrate_bps = 2500000;
  gcc.aimd.ack_bitrate_bps = 1800000;
  gcc.aimd.have_ack_bitrate = true;
  gcc.aimd.ack_window_bytes = 4096;
  gcc.trendline.history_count = 3;
  gcc.current_group.packet_count = 2;
  session.gcc_ctx = &gcc;
  gcc_bwe_context_t gcc_before = gcc;

  atomic_store(&session.egress_generation, 7);

  sfu_layer_selector_switch_source(&session, 42);

  /* The selector re-aims at the per-publisher scheduler for source 42, reset
   * with the keyframe gate armed. */
  sfu_subscriber_scheduler_t *sw = sfu_session_scheduler_for(&session, 42);
  assert(sw != NULL);
  assert(sw->active_publisher_id == 42);
  assert(sw->needs_keyframe == true);   /* gate armed */
  assert(sw->current_sid == 0 && sw->current_tid == 0);
  assert(atomic_load(&session.egress_generation) == 8); /* stale RTX invalidated */
  assert(memcmp(&gcc, &gcc_before, sizeof(gcc)) == 0);
}

static sfu_svc_descriptor_t make_desc(uint32_t timestamp, uint8_t sid, uint8_t tid, uint8_t p, uint8_t u, uint8_t d, uint8_t b, uint8_t e) {
  sfu_svc_descriptor_t desc = {0};
  desc.rtp_timestamp = timestamp;
  desc.sid = sid;
  desc.tid = tid;
  desc.p_bit = p;
  desc.u_bit = u;
  desc.d_bit = d;
  desc.b_bit = b;
  desc.e_bit = e;
  return desc;
}

static void test_pacer_classification(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sfu_svc_descriptor_t desc = make_desc(1, 0, 0, 1, 0, 0, 1, 1);
  assert(sfu_scheduler_classify_frame(&sched, &desc) == SFU_PACER_CLASS_VIDEO_BASE);
  desc.tid = 1;
  assert(sfu_scheduler_classify_frame(&sched, &desc) == SFU_PACER_CLASS_VIDEO_ENH);
  desc.tid = 0;
  desc.sid = 1;
  assert(sfu_scheduler_classify_frame(&sched, &desc) == SFU_PACER_CLASS_VIDEO_ENH);
}

static void test_spatial_dependency_requires_completed_lower_layer(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sched.target_tid = 0;

  sfu_scheduler_decision_t decision;
  sfu_svc_descriptor_t upper = make_desc(100, 1, 0, 0, 0, 1, 1, 1);
  assert(!sfu_scheduler_prepare_packet(&sched, &upper, false, &decision));
  assert(sched.current_sid == 0);

  sfu_svc_descriptor_t lower = make_desc(100, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_scheduler_prepare_packet(&sched, &lower, true, &decision));
  assert(decision.pacer_class == SFU_PACER_CLASS_VIDEO_TRANSITION);
  sfu_scheduler_commit_packet(&sched, &decision);

  assert(sfu_scheduler_prepare_packet(&sched, &upper, false, &decision));
  assert(sched.current_sid == 0);
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_sid == 1);
}

static void test_spatial_transition_reject_prevents_promotion(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sched.target_tid = 0;

  sfu_scheduler_decision_t decision;
  sfu_svc_descriptor_t lower = make_desc(200, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_scheduler_prepare_packet(&sched, &lower, true, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);

  sfu_svc_descriptor_t start = make_desc(200, 1, 0, 0, 0, 1, 1, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &start, false, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.transition_active);

  sfu_svc_descriptor_t middle = make_desc(200, 1, 0, 0, 0, 1, 0, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &middle, false, &decision));
  sfu_scheduler_reject_packet(&sched, &decision);

  sfu_svc_descriptor_t end = make_desc(200, 1, 0, 0, 0, 1, 0, 1);
  assert(!sfu_scheduler_prepare_packet(&sched, &end, false, &decision));
  assert(sched.current_sid == 0);
}

static void test_independent_spatial_transition_and_picture_reset(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sched.target_tid = 0;

  sfu_scheduler_decision_t decision;
  sfu_svc_descriptor_t independent = make_desc(300, 1, 0, 0, 0, 0, 1, 1);
  assert(sfu_scheduler_prepare_packet(&sched, &independent, false, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_sid == 1);

  sfu_subscriber_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sfu_svc_descriptor_t lower = make_desc(400, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_scheduler_prepare_packet(&sched, &lower, true, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);
  sfu_svc_descriptor_t upper_next_picture = make_desc(401, 1, 0, 0, 0, 1, 1, 1);
  assert(!sfu_scheduler_prepare_packet(&sched, &upper_next_picture, false, &decision));
}

static void test_keyframe_gate_does_not_jump_to_target(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sched.target_sid = 2;
  sched.target_tid = 2;

  sfu_scheduler_decision_t decision;
  sfu_svc_descriptor_t key = make_desc(500, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_scheduler_prepare_packet(&sched, &key, true, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);
  assert(sched.current_sid == 0);
  assert(sched.current_tid == 0);
}

static void test_multi_packet_keyframe_transaction(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sched.target_sid = 0;
  sched.target_tid = 0;

  sfu_scheduler_decision_t decision;
  sfu_svc_descriptor_t start = make_desc(600, 0, 0, 0, 0, 0, 1, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &start, true, &decision));
  assert(decision.start_keyframe && decision.keyframe_packet);
  assert(!decision.set_marker);
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.needs_keyframe && sched.keyframe_active);

  sfu_svc_descriptor_t middle = make_desc(600, 0, 0, 0, 0, 0, 0, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &middle, false, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.needs_keyframe);

  sfu_svc_descriptor_t end = make_desc(600, 0, 0, 0, 0, 0, 0, 1);
  assert(sfu_scheduler_prepare_packet(&sched, &end, false, &decision));
  assert(decision.set_marker);
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe && !sched.keyframe_active);
}

static void test_keyframe_reject_keeps_gate_armed(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);

  sfu_scheduler_decision_t decision;
  sfu_svc_descriptor_t start = make_desc(700, 0, 0, 0, 0, 0, 1, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &start, true, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);

  sfu_svc_descriptor_t middle = make_desc(700, 0, 0, 0, 0, 0, 0, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &middle, false, &decision));
  sfu_scheduler_reject_packet(&sched, &decision);
  assert(sched.needs_keyframe && sched.keyframe_failed);

  sfu_svc_descriptor_t end = make_desc(700, 0, 0, 0, 0, 0, 0, 1);
  assert(!sfu_scheduler_prepare_packet(&sched, &end, false, &decision));
}

static void test_temporal_transition_commits_on_end(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 0;
  sched.target_tid = 1;

  sfu_scheduler_decision_t decision;
  sfu_svc_descriptor_t start = make_desc(800, 0, 1, 1, 1, 0, 1, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &start, false, &decision));
  assert(decision.start_temporal_transition);
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_tid == 0 && sched.temporal_transition_active);

  sfu_svc_descriptor_t middle = make_desc(800, 0, 1, 1, 0, 0, 0, 0);
  assert(sfu_scheduler_prepare_packet(&sched, &middle, false, &decision));
  assert(decision.pacer_class == SFU_PACER_CLASS_VIDEO_TRANSITION);
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_tid == 0);

  sfu_svc_descriptor_t end = make_desc(800, 0, 1, 1, 0, 0, 0, 1);
  assert(sfu_scheduler_prepare_packet(&sched, &end, false, &decision));
  sfu_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_tid == 1 && !sched.temporal_transition_active);
}

static void test_output_sequence_translation(void) {
  sfu_subscriber_scheduler_t sched;
  sfu_subscriber_scheduler_init(&sched, 1);
  assert(sfu_scheduler_assign_output_seq(&sched, 100) == 100);
  assert(sfu_scheduler_assign_output_seq(&sched, 102) == 101);
  sched.next_output_seq = UINT16_MAX;
  assert(sfu_scheduler_assign_output_seq(&sched, 500) == UINT16_MAX);
  assert(sfu_scheduler_assign_output_seq(&sched, 600) == 0);
}

int main(void) {
  test_up_needs_headroom();
  test_down_holds_at_rung_rate();
  test_dwell_blocks_fast_flap();
  test_switch_source_transaction();
  test_pacer_classification();
  test_spatial_dependency_requires_completed_lower_layer();
  test_spatial_transition_reject_prevents_promotion();
  test_independent_spatial_transition_and_picture_reset();
  test_keyframe_gate_does_not_jump_to_target();
  test_multi_packet_keyframe_transaction();
  test_keyframe_reject_keeps_gate_armed();
  test_temporal_transition_commits_on_end();
  test_output_sequence_translation();
  printf("test_layer_selector: OK\n");
  return 0;
}
