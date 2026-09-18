/* Layer selector tests (#83): hysteresis, dwell, source-switch transaction. */

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "congestion/gcc.h"
#include "media/svc/layer_scheduler.h"
#include "peer/session.h"
#include "sfu/datadef.h"

#define DWELL_EXPIRE(s_) ((s_).last_target_change_us -= 3500000LL)

static void test_l1t3_bitrate_ladder_stays_on_spatial_zero(void) {
  sfu_layer_scheduler_t s;
  sfu_layer_scheduler_init(&s, 1);

  s.target_sid = 0;
  s.target_tid = 0;

  sfu_layer_scheduler_set_bitrate(&s, 1200000);
  assert(s.target_sid == 0 && s.target_tid != 2);
  DWELL_EXPIRE(s);
  sfu_layer_scheduler_set_bitrate(&s, 1440000);
  assert(s.target_sid == 0 && s.target_tid == 2);
}

static void test_down_holds_at_rung_rate(void) {
  sfu_layer_scheduler_t s;
  sfu_layer_scheduler_init(&s, 1);

  s.target_sid = 0;
  s.target_tid = 0;
  sfu_layer_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 0 && s.target_tid == 2);

  /* Falling between rung rate and up threshold holds the rung. */
  DWELL_EXPIRE(s);
  sfu_layer_scheduler_set_bitrate(&s, 1300000);
  assert(s.target_sid == 0 && s.target_tid == 2);

  /* Breaking the down threshold drops immediately without dwell. */
  sfu_layer_scheduler_set_bitrate(&s, 1100000);
  assert(s.target_sid == 0 && s.target_tid == 1);
}

static void test_dwell_blocks_fast_flap(void) {
  sfu_layer_scheduler_t s;
  sfu_layer_scheduler_init(&s, 1);

  s.target_sid = 0;
  s.target_tid = 0;
  sfu_layer_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 0 && s.target_tid == 2);
  assert(s.last_target_change_us != 0);

  /* Congestion: immediate downswitch to T0 */
  sfu_layer_scheduler_set_bitrate(&s, 100000);
  assert(s.target_sid == 0 && s.target_tid == 0);
  assert(s.allocated_bps == 100000);

  /* Network flaps up immediately: upgrade blocked by 3-second hold-down */
  sfu_layer_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 0 && s.target_tid == 0);

  /* After 3 seconds: upgrade succeeds to T2 */
  DWELL_EXPIRE(s);
  sfu_layer_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 0 && s.target_tid == 2);
}

static void test_camera_and_screen_allocations_are_independent(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP] = {0};
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);

  sfu_layer_scheduler_t *camera = sfu_layer_scheduler_for_stream(&session, 77, SFU_MEDIA_VIDEO);
  sfu_layer_scheduler_t *screen = sfu_layer_scheduler_for_stream(&session, 77, SFU_MEDIA_SCREEN);
  assert(camera != NULL && screen != NULL && camera != screen);
  camera->target_tid = 0;
  screen->target_tid = 0;

  sfu_layer_scheduler_set_bitrate(camera, 240000);
  sfu_layer_scheduler_set_bitrate(screen, 1440000);
  assert(camera->allocated_bps == 240000 && camera->target_tid == 0);
  assert(screen->allocated_bps == 1440000 && screen->target_tid == 2);

  sfu_layer_scheduler_set_bitrate(screen, 100000);
  assert(screen->allocated_bps == 100000);
  assert(screen->target_tid == 2); /* screen share preserves all temporal layers (T2) */
  assert(camera->allocated_bps == 240000 && camera->target_tid == 0);
}

static void test_switch_source_transaction(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  static sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP];
  memset(slots, 0, sizeof(slots));
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);
  /* Pre-existing per-publisher state for source 1 (the source being left). */
  sfu_layer_scheduler_t *old = sfu_layer_scheduler_for(&session, 1);
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
  session.egress.gcc_ctx = &gcc;
  gcc_bwe_context_t gcc_before = gcc;

  atomic_store(&session.egress.generation, 7);

  sfu_layer_scheduler_switch_source(&session, 42);

  /* The selector re-aims at the per-publisher scheduler for source 42, reset
   * with the keyframe gate armed. */
  sfu_layer_scheduler_t *sw = sfu_layer_scheduler_for(&session, 42);
  assert(sw != NULL);
  assert(sw->active_publisher_id == 42);
  assert(sw->needs_keyframe == true); /* gate armed */
  assert(sw->current_sid == 0 && sw->current_tid == 0);
  assert(atomic_load(&session.egress.generation) == 8); /* stale RTX invalidated */
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
  desc.l_bit = 1;
  return desc;
}

static void test_pacer_classification(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sfu_svc_descriptor_t desc = make_desc(1, 0, 0, 1, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_classify_frame(&sched, &desc) == SFU_PACER_CLASS_VIDEO_BASE);
  desc.tid = 1;
  assert(sfu_layer_scheduler_classify_frame(&sched, &desc) == SFU_PACER_CLASS_VIDEO_ENH);
  desc.tid = 0;
  desc.sid = 1;
  assert(sfu_layer_scheduler_classify_frame(&sched, &desc) == SFU_PACER_CLASS_VIDEO_ENH);
}

static void test_spatial_dependency_requires_completed_lower_layer(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sched.target_tid = 0;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t upper = make_desc(100, 1, 0, 0, 0, 1, 1, 1);
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &upper, false, &decision));
  assert(decision.reject_reason == SFU_LAYER_REJECT_SPATIAL_DEPENDENCY);
  assert(sched.current_sid == 0);

  sfu_svc_descriptor_t lower = make_desc(100, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &lower, true, &decision));
  assert(decision.reject_reason == SFU_LAYER_REJECT_NONE);
  assert(decision.pacer_class == SFU_PACER_CLASS_VIDEO_TRANSITION);
  sfu_layer_scheduler_commit_packet(&sched, &decision);

  assert(sfu_layer_scheduler_prepare_packet(&sched, &upper, false, &decision));
  assert(sched.current_sid == 0);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_sid == 1);
}

static void test_spatial_transition_reject_prevents_promotion(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sched.target_tid = 0;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t lower = make_desc(200, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &lower, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);

  sfu_svc_descriptor_t start = make_desc(200, 1, 0, 0, 0, 1, 1, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &start, false, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.transition_active);

  sfu_svc_descriptor_t middle = make_desc(200, 1, 0, 0, 0, 1, 0, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &middle, false, &decision));
  sfu_layer_scheduler_reject_packet(&sched, &decision);

  sfu_svc_descriptor_t end = make_desc(200, 1, 0, 0, 0, 1, 0, 1);
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &end, false, &decision));
  assert(decision.reject_reason == SFU_LAYER_REJECT_OVER_TARGET_OR_FAILED);
  assert(sched.current_sid == 0);
}

static void test_independent_spatial_transition_and_picture_reset(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sched.target_tid = 0;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t independent = make_desc(300, 1, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &independent, false, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_sid == 1);

  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 1;
  sfu_svc_descriptor_t lower = make_desc(400, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &lower, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  sfu_svc_descriptor_t upper_next_picture = make_desc(401, 1, 0, 0, 0, 1, 1, 1);
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &upper_next_picture, false, &decision));
}

static void test_keyframe_gate_does_not_jump_to_target(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.target_sid = 2;
  sched.target_tid = 2;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t key = make_desc(500, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &key, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);
  assert(sched.current_sid == 0);
  assert(sched.current_tid == 0);
}

static void test_l1t1_10fps_multi_packet_delta_frame(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 0;
  sched.target_tid = 0;

  sfu_layer_scheduler_decision_t decision;
  const uint32_t first_timestamp = 90000;
  for (int frame = 0; frame < 2; frame++) {
    uint32_t timestamp = first_timestamp + (uint32_t)frame * 9000;
    for (int packet = 0; packet < 5; packet++) {
      sfu_svc_descriptor_t desc = make_desc(timestamp, 0, 0, 1, 0, 0, packet == 0, packet == 4);
      assert(sfu_layer_scheduler_prepare_packet(&sched, &desc, false, &decision));
      assert(decision.pacer_class == (packet == 0 ? SFU_PACER_CLASS_VIDEO_BASE : SFU_PACER_CLASS_VIDEO_TRANSITION));
      assert(decision.set_marker == (packet == 4));
      sfu_layer_scheduler_commit_packet(&sched, &decision);
    }
  }
}

static void test_multi_packet_keyframe_transaction(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.target_sid = 0;
  sched.target_tid = 0;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t start = make_desc(600, 0, 0, 0, 0, 0, 1, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &start, true, &decision));
  assert(decision.start_keyframe && decision.keyframe_packet);
  assert(!decision.set_marker);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.needs_keyframe && sched.keyframe_active);

  for (int packet = 0; packet < 32; packet++) {
    sfu_svc_descriptor_t middle = make_desc(600, 0, 0, 0, 0, 0, 0, 0);
    assert(sfu_layer_scheduler_prepare_packet(&sched, &middle, false, &decision));
    assert(decision.pacer_class == SFU_PACER_CLASS_VIDEO_TRANSITION);
    assert(!decision.set_marker);
    sfu_layer_scheduler_commit_packet(&sched, &decision);
    assert(sched.needs_keyframe);
  }

  sfu_svc_descriptor_t end = make_desc(600, 0, 0, 0, 0, 0, 0, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &end, false, &decision));
  assert(decision.set_marker);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe && !sched.keyframe_active);
}

static void test_keyframe_reject_keeps_gate_armed(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t start = make_desc(700, 0, 0, 0, 0, 0, 1, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &start, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);

  sfu_svc_descriptor_t middle = make_desc(700, 0, 0, 0, 0, 0, 0, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &middle, false, &decision));
  sfu_layer_scheduler_reject_packet(&sched, &decision);
  assert(sched.needs_keyframe && sched.keyframe_failed);

  sfu_svc_descriptor_t end = make_desc(700, 0, 0, 0, 0, 0, 0, 1);
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &end, false, &decision));

  sfu_svc_descriptor_t recovery = make_desc(9000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &recovery, true, &decision));
  assert(decision.pacer_class == SFU_PACER_CLASS_VIDEO_TRANSITION);
  assert(decision.set_marker);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe && !sched.keyframe_failed);
}

static void test_temporal_transition_commits_on_end(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.target_sid = 0;
  sched.target_tid = 1;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t start = make_desc(800, 0, 1, 1, 1, 0, 1, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &start, false, &decision));
  assert(decision.start_temporal_transition);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_tid == 0 && sched.temporal_transition_active);

  sfu_svc_descriptor_t middle = make_desc(800, 0, 1, 1, 0, 0, 0, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &middle, false, &decision));
  assert(decision.pacer_class == SFU_PACER_CLASS_VIDEO_TRANSITION);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_tid == 0);

  sfu_svc_descriptor_t end = make_desc(800, 0, 1, 1, 0, 0, 0, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &end, false, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.current_tid == 1 && !sched.temporal_transition_active);
}

static void test_enhancement_frame_admission_latches_until_end(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.current_tid = 1;
  sched.target_tid = 1;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t start = make_desc(900, 0, 1, 1, 0, 0, 1, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &start, false, &decision));
  assert(decision.pacer_frame_start && !decision.pacer_frame_end);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(sched.pacer_frame_active);

  sfu_svc_descriptor_t middle = make_desc(900, 0, 1, 1, 0, 0, 0, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &middle, false, &decision));
  assert(decision.pacer_frame_continuation);
  sfu_layer_scheduler_commit_packet(&sched, &decision);

  sfu_svc_descriptor_t end = make_desc(900, 0, 1, 1, 0, 0, 0, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &end, false, &decision));
  assert(decision.pacer_frame_continuation && decision.pacer_frame_end);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.pacer_frame_active);

  sfu_layer_scheduler_init(&sched, 1);
  sched.needs_keyframe = false;
  sched.current_tid = 1;
  sched.target_tid = 1;
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &middle, false, &decision));
  assert(decision.pacer_frame_continuation);
}

static void test_screen_share_admits_higher_tid_without_u_bit(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.source = SFU_MEDIA_SCREEN;

  sfu_layer_scheduler_decision_t decision;
  /* Keyframe at timestamp 1000 */
  sfu_svc_descriptor_t kf = make_desc(1000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &kf, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);
  assert(sched.target_tid == 2 && sched.current_tid == 2);

  /* Delta frame with TID=1 and u_bit=0 (standard WebRTC VP9 screen delta) */
  sfu_svc_descriptor_t delta_tid1 = make_desc(2000, 0, 1, 1, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &delta_tid1, false, &decision));
  assert(decision.should_forward);
  assert(decision.reject_reason == SFU_LAYER_REJECT_NONE);
  sfu_layer_scheduler_commit_packet(&sched, &decision);

  /* Delta frame with TID=2 and u_bit=0 */
  sfu_svc_descriptor_t delta_tid2 = make_desc(3000, 0, 2, 1, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &delta_tid2, false, &decision));
  assert(decision.should_forward);
  assert(decision.reject_reason == SFU_LAYER_REJECT_NONE);
  sfu_layer_scheduler_commit_packet(&sched, &decision);
}

static void test_screen_share_upgrades_immediately_without_dwell(void) {
  sfu_layer_scheduler_t s;
  sfu_layer_scheduler_init(&s, 1);
  s.source = SFU_MEDIA_SCREEN;

  s.target_sid = 0;
  s.target_tid = 0;
  sfu_layer_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 0 && s.target_tid == 2);
  assert(s.last_target_change_us != 0);

  /* Congestion: immediate downswitch */
  sfu_layer_scheduler_set_bitrate(&s, 100000);
  assert(s.target_sid == 0 && s.target_tid == 2); /* screen always forces tid=2 */

  /* Screen share: upgrade applies immediately (no 3s dwell) */
  sfu_layer_scheduler_set_bitrate(&s, 2000000);
  assert(s.target_sid == 0 && s.target_tid == 2);
}

static void test_audio_does_not_consume_slot(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP] = {0};
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);

  sfu_layer_scheduler_t *sched = sfu_layer_scheduler_for_stream(&session, 42, SFU_MEDIA_AUDIO);
  assert(sched == NULL);
  for (uint32_t i = 0; i < SFU_LAYER_SCHEDULER_CAP; i++) {
    assert(slots[i].publisher_id == 0);
  }
}

static void test_full_table_rejection_and_prune_reclaims_slot(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP] = {0};
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);

  /* Fill all 16 slots with 16 distinct publishers */
  for (uint32_t i = 1; i <= SFU_LAYER_SCHEDULER_CAP; i++) {
    sfu_layer_scheduler_t *sched = sfu_layer_scheduler_for_stream(&session, i, SFU_MEDIA_VIDEO);
    assert(sched != NULL);
  }

  /* 17th stream must be rejected when table is full */
  sfu_layer_scheduler_t *sched17 = sfu_layer_scheduler_for_stream(&session, 17, SFU_MEDIA_VIDEO);
  assert(sched17 == NULL);

  /* Prepare snapshot with only publishers 1..15 active (publisher 16 departed) */
  sfu_peer_session_t dummy_sub;
  memset(&dummy_sub, 0, sizeof(dummy_sub));
  atomic_store(&dummy_sub.refcount, 1000);

  sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_alloc();
  assert(snap != NULL);
  for (uint32_t i = 1; i < SFU_LAYER_SCHEDULER_CAP; i++) {
    sfu_receiver_entry_t entry = {
        .subscriber = &dummy_sub,
        .publisher_peer_id = i,
        .has_video = true,
        .video_active = true,
    };
    assert(sfu_receiver_snapshot_set(snap, i - 1, &entry));
  }

  /* Prune releases slot for departed publisher 16 */
  sfu_layer_scheduler_prune(&session, snap);

  /* Verify publisher 16's slot was reclaimed (publisher_id == 0) */
  bool found_free = false;
  for (uint32_t i = 0; i < SFU_LAYER_SCHEDULER_CAP; i++) {
    if (slots[i].publisher_id == 0) {
      found_free = true;
      break;
    }
  }
  assert(found_free);

  /* Now stream 17 can successfully acquire a slot */
  sched17 = sfu_layer_scheduler_for_stream(&session, 17, SFU_MEDIA_VIDEO);
  assert(sched17 != NULL);
  assert(sched17->active_publisher_id == 17);
  assert(sched17->needs_keyframe == true);

  sfu_subscriptions_snapshot_release(snap);
}

static void test_full_state_reset_on_reuse(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP] = {0};
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);

  sfu_layer_scheduler_t *sched = sfu_layer_scheduler_for_stream(&session, 10, SFU_MEDIA_VIDEO);
  assert(sched != NULL);

  /* Pollute scheduler fields with active stream state */
  sched->target_sid = 2;
  sched->target_tid = 2;
  sched->current_sid = 2;
  sched->current_tid = 2;
  sched->allocated_bps = 800000;
  sched->needs_keyframe = false;
  sched->picture_valid = true;
  sched->picture_timestamp = 12345;
  sched->transition_active = true;
  sched->temporal_transition_active = true;
  sched->pacer_frame_active = true;

  /* Explicit slot reset */
  sfu_layer_scheduler_slot_reset(&slots[0]);
  assert(slots[0].publisher_id == 0);
  assert(slots[0].sched.target_sid == 0);
  assert(slots[0].sched.target_tid == 0);
  assert(slots[0].sched.current_sid == 0);
  assert(slots[0].sched.current_tid == 0);
  assert(slots[0].sched.allocated_bps == 0);
  assert(slots[0].sched.needs_keyframe == false);
  assert(slots[0].sched.picture_valid == false);
  assert(slots[0].sched.transition_active == false);
  assert(slots[0].sched.temporal_transition_active == false);
  assert(slots[0].sched.pacer_frame_active == false);

  /* Reuse the slot for publisher 20 */
  sfu_layer_scheduler_t *new_sched = sfu_layer_scheduler_for_stream(&session, 20, SFU_MEDIA_VIDEO);
  assert(new_sched == &slots[0].sched);
  assert(new_sched->active_publisher_id == 20);
  assert(new_sched->needs_keyframe == true);
  assert(new_sched->target_sid == 0);
  assert(new_sched->target_tid == 2);
  assert(new_sched->current_sid == 0);
  assert(new_sched->allocated_bps == 0);
  assert(!new_sched->transition_active);
}

static void test_independent_camera_and_screen_pruning(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP] = {0};
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);

  sfu_layer_scheduler_t *cam = sfu_layer_scheduler_for_stream(&session, 50, SFU_MEDIA_VIDEO);
  sfu_layer_scheduler_t *scr = sfu_layer_scheduler_for_stream(&session, 50, SFU_MEDIA_SCREEN);
  assert(cam != NULL && scr != NULL);

  uint64_t cam_key = ((50ULL) << 8) | SFU_MEDIA_VIDEO;
  uint64_t scr_key = ((50ULL) << 8) | SFU_MEDIA_SCREEN;
  assert(slots[0].publisher_id == cam_key);
  assert(slots[1].publisher_id == scr_key);

  sfu_peer_session_t dummy_sub;
  memset(&dummy_sub, 0, sizeof(dummy_sub));
  atomic_store(&dummy_sub.refcount, 1000);

  /* Snapshot 1: camera active, screen inactive */
  sfu_receiver_snapshot_t *snap1 = sfu_receiver_snapshot_alloc();
  assert(snap1 != NULL);
  sfu_receiver_entry_t entry1 = {
      .subscriber = &dummy_sub,
      .publisher_peer_id = 50,
      .has_video = true,
      .video_active = true,
      .has_screen = true,
      .screen_active = false,
  };
  assert(sfu_receiver_snapshot_set(snap1, 0, &entry1));

  sfu_layer_scheduler_prune(&session, snap1);

  /* Camera slot remains, screen slot is reclaimed */
  assert(slots[0].publisher_id == cam_key);
  assert(slots[1].publisher_id == 0);

  /* Snapshot 2: both inactive */
  sfu_receiver_snapshot_t *snap2 = sfu_receiver_snapshot_alloc();
  assert(snap2 != NULL);
  sfu_receiver_entry_t entry2 = {
      .subscriber = &dummy_sub,
      .publisher_peer_id = 50,
      .has_video = true,
      .video_active = false,
      .has_screen = true,
      .screen_active = false,
  };
  assert(sfu_receiver_snapshot_set(snap2, 0, &entry2));

  sfu_layer_scheduler_prune(&session, snap2);

  /* Now camera slot is also reclaimed */
  assert(slots[0].publisher_id == 0);
  assert(slots[1].publisher_id == 0);

  sfu_subscriptions_snapshot_release(snap1);
  sfu_subscriptions_snapshot_release(snap2);
}

static void test_preservation_of_active_entries_with_zero_bitrate(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP] = {0};
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);

  sfu_layer_scheduler_t *cam = sfu_layer_scheduler_for_stream(&session, 60, SFU_MEDIA_VIDEO);
  assert(cam != NULL);
  sfu_layer_scheduler_set_bitrate(cam, 0);
  assert(cam->allocated_bps == 0);

  uint64_t cam_key = ((60ULL) << 8) | SFU_MEDIA_VIDEO;

  sfu_peer_session_t dummy_sub;
  memset(&dummy_sub, 0, sizeof(dummy_sub));
  atomic_store(&dummy_sub.refcount, 1000);

  /* Snapshot has publisher 60 video active, even though bitrate is 0 */
  sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_alloc();
  assert(snap != NULL);
  sfu_receiver_entry_t entry = {
      .subscriber = &dummy_sub,
      .publisher_peer_id = 60,
      .has_video = true,
      .video_active = true,
  };
  assert(sfu_receiver_snapshot_set(snap, 0, &entry));

  sfu_layer_scheduler_prune(&session, snap);

  /* Active stream must NOT be evicted even if allocated_bps == 0 */
  assert(slots[0].publisher_id == cam_key);
  assert(slots[0].sched.allocated_bps == 0);

  /* NULL snapshot must not clear slots either */
  sfu_layer_scheduler_prune(&session, NULL);
  assert(slots[0].publisher_id == cam_key);

  sfu_subscriptions_snapshot_release(snap);
}

static void test_repeated_reconciliation_without_leaks(void) {
  sfu_peer_session_t session;
  memset(&session, 0, sizeof(session));
  sfu_layer_scheduler_slot_t slots[SFU_LAYER_SCHEDULER_CAP] = {0};
  session.egress.schedulers = slots;
  atomic_store(&session.egress.video_runtime_state, SFU_VIDEO_RUNTIME_READY);

  sfu_peer_session_t dummy_sub;
  memset(&dummy_sub, 0, sizeof(dummy_sub));
  atomic_store(&dummy_sub.refcount, 10000);

  /* Simulate 50 reconnect cycles of a single camera publisher */
  for (uint32_t peer_id = 1; peer_id <= 50; peer_id++) {
    sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_alloc();
    assert(snap != NULL);
    sfu_receiver_entry_t entry = {
        .subscriber = &dummy_sub,
        .publisher_peer_id = peer_id,
        .has_video = true,
        .video_active = true,
    };
    assert(sfu_receiver_snapshot_set(snap, 0, &entry));

    /* Prune stale slots from previous peer */
    sfu_layer_scheduler_prune(&session, snap);

    /* Allocate slot for current peer */
    sfu_layer_scheduler_t *sched = sfu_layer_scheduler_for_stream(&session, peer_id, SFU_MEDIA_VIDEO);
    assert(sched != NULL);
    sfu_layer_scheduler_set_bitrate(sched, 500000);

    /* Count occupied slots: exactly 1 slot must be occupied */
    uint32_t occupied = 0;
    for (uint32_t i = 0; i < SFU_LAYER_SCHEDULER_CAP; i++) {
      if (slots[i].publisher_id != 0) {
        occupied++;
      }
    }
    assert(occupied == 1);

    sfu_subscriptions_snapshot_release(snap);
  }
}

static void test_screen_share_reject_arms_needs_keyframe(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.source = SFU_MEDIA_SCREEN;

  sfu_layer_scheduler_decision_t decision;
  /* Keyframe at timestamp 1000 */
  sfu_svc_descriptor_t kf = make_desc(1000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &kf, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);

  /* Delta frame at timestamp 2000 */
  sfu_svc_descriptor_t delta = make_desc(2000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &delta, false, &decision));
  /* Packet rejected in egress (e.g. backlog bound or pacing drop) */
  sfu_layer_scheduler_reject_packet(&sched, &decision);

  /* For screen share, dropping a delta frame breaks the decoder reference chain,
   * so reject_packet must set needs_keyframe = true */
  assert(sched.needs_keyframe);

  /* Subsequent delta frame at timestamp 3000 must be suppressed */
  sfu_svc_descriptor_t delta2 = make_desc(3000, 0, 0, 0, 0, 0, 1, 1);
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &delta2, false, &decision));
  assert(decision.reject_reason == SFU_LAYER_REJECT_KEYFRAME_REQUIRED);

  /* Fresh keyframe at timestamp 4000 recovers */
  sfu_svc_descriptor_t kf2 = make_desc(4000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &kf2, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);
}

static void test_camera_video_reject_arms_needs_keyframe(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.source = SFU_MEDIA_VIDEO;

  sfu_layer_scheduler_decision_t decision;
  /* Keyframe at timestamp 1000 */
  sfu_svc_descriptor_t kf = make_desc(1000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &kf, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);

  /* Delta frame at timestamp 2000 on active spatial layer 0 */
  sfu_svc_descriptor_t delta = make_desc(2000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &delta, false, &decision));
  /* Rejected due to queue delay/drop */
  sfu_layer_scheduler_reject_packet(&sched, &decision);

  /* For camera video, dropping an active layer delta packet must arm needs_keyframe */
  assert(sched.needs_keyframe);

  /* Next delta frame at timestamp 3000 must be gated */
  sfu_svc_descriptor_t delta2 = make_desc(3000, 0, 0, 0, 0, 0, 1, 1);
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &delta2, false, &decision));
  assert(decision.reject_reason == SFU_LAYER_REJECT_KEYFRAME_REQUIRED);

  /* Keyframe at timestamp 4000 recovers */
  sfu_svc_descriptor_t kf2 = make_desc(4000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &kf2, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);
}

static void test_incomplete_picture_timestamp_transition_arms_needs_keyframe(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.source = SFU_MEDIA_SCREEN;

  sfu_layer_scheduler_decision_t decision;
  /* Keyframe at timestamp 1000 completes cleanly */
  sfu_svc_descriptor_t kf = make_desc(1000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &kf, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);

  /* Delta frame at timestamp 2000 with 2 packets: packet 1 has b_bit=1, e_bit=0 */
  sfu_svc_descriptor_t pkt1 = make_desc(2000, 0, 0, 0, 0, 0, 1, 0);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &pkt1, false, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);

  /* Packet 2 is lost/dropped. Next packet belongs to timestamp 3000 */
  sfu_svc_descriptor_t pkt_next = make_desc(3000, 0, 0, 0, 0, 0, 1, 1);
  /* Preparing pkt_next triggers layer_scheduler_begin_picture(3000) which detects
   * timestamp 2000 was started but never completed (missing e_bit), arming needs_keyframe */
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &pkt_next, false, &decision));
  assert(sched.needs_keyframe);
  assert(decision.reject_reason == SFU_LAYER_REJECT_KEYFRAME_REQUIRED);
}

static void test_camera_over_target_must_not_arm_keyframe(void) {
  sfu_layer_scheduler_t sched;
  sfu_layer_scheduler_init(&sched, 1);
  sched.source = SFU_MEDIA_VIDEO;

  sfu_layer_scheduler_decision_t decision;
  sfu_svc_descriptor_t kf = make_desc(1000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &kf, true, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);
  /* target/current remain 0/0 so sid=1 is an unselected enhancement layer.
   * Dropping it is normal bandwidth adaptation and must NOT trigger a PLI. */
  assert(sched.target_sid == 0 && sched.current_sid == 0);

  sfu_svc_descriptor_t enh = make_desc(2000, 1, 0, 0, 0, 1, 1, 1);
  assert(!sfu_layer_scheduler_prepare_packet(&sched, &enh, false, &decision));
  assert(decision.reject_reason == SFU_LAYER_REJECT_OVER_TARGET_OR_FAILED);
  sfu_layer_scheduler_reject_packet(&sched, &decision);
  assert(!sched.needs_keyframe);

  /* Active-layer deltas must still be gated — proves the negative case is
   * narrowly scoped to over-target enhancement, not a blanket suppression. */
  sfu_svc_descriptor_t delta = make_desc(3000, 0, 0, 0, 0, 0, 1, 1);
  assert(sfu_layer_scheduler_prepare_packet(&sched, &delta, false, &decision));
  sfu_layer_scheduler_commit_packet(&sched, &decision);
  assert(!sched.needs_keyframe);
}

int main(void) {
  test_l1t3_bitrate_ladder_stays_on_spatial_zero();
  test_down_holds_at_rung_rate();
  test_dwell_blocks_fast_flap();
  test_camera_and_screen_allocations_are_independent();
  test_switch_source_transaction();
  test_pacer_classification();
  test_spatial_dependency_requires_completed_lower_layer();
  test_spatial_transition_reject_prevents_promotion();
  test_independent_spatial_transition_and_picture_reset();
  test_keyframe_gate_does_not_jump_to_target();
  test_l1t1_10fps_multi_packet_delta_frame();
  test_multi_packet_keyframe_transaction();
  test_keyframe_reject_keeps_gate_armed();
  test_temporal_transition_commits_on_end();
  test_enhancement_frame_admission_latches_until_end();
  test_screen_share_admits_higher_tid_without_u_bit();
  test_screen_share_upgrades_immediately_without_dwell();
  test_screen_share_reject_arms_needs_keyframe();
  test_camera_video_reject_arms_needs_keyframe();
  test_incomplete_picture_timestamp_transition_arms_needs_keyframe();
  test_camera_over_target_must_not_arm_keyframe();
  test_audio_does_not_consume_slot();
  test_full_table_rejection_and_prune_reclaims_slot();
  test_full_state_reset_on_reuse();
  test_independent_camera_and_screen_pruning();
  test_preservation_of_active_entries_with_zero_bitrate();
  test_repeated_reconciliation_without_leaks();
  printf("test_layer_selector: OK\n");
  return 0;
}
