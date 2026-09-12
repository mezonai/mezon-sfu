#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "congestion/gcc.h"
#include "congestion/probe_controller.h"
#include "util/metrics.h"

static void test_lifecycle_and_reset(void) {
  sfu_probe_controller_t *pc = sfu_probe_controller_create();
  assert(pc != NULL);
  assert(!sfu_probe_controller_is_probing(pc));

  sfu_probe_controller_set_unmet_demand(pc, true);
  assert(pc->has_unmet_demand);

  sfu_probe_controller_reset(pc);
  assert(!sfu_probe_controller_is_probing(pc));
  assert(pc->bytes_sent == 0);
  assert(pc->packets_sent == 0);

  sfu_probe_controller_destroy(pc);
}

static void test_eligibility_and_conditions(void) {
  sfu_probe_controller_t pc;
  sfu_probe_controller_init(&pc);

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 300000u, 50000u, 5000000u);

  int64_t now_us = 1000000LL;
  gcc_bwe_record_feedback(&gcc, now_us);

  /* Initially no unmet demand -> cannot probe */
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us, true, true, 0, 0));

  /* Enable unmet demand */
  sfu_probe_controller_set_unmet_demand(&pc, true);
  assert(sfu_probe_controller_should_probe(&pc, &gcc, now_us, true, true, 0, 0));

  /* No TWCC negotiated -> cannot probe */
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us, false, true, 0, 0));

  /* No RTX SSRC negotiated -> cannot probe (would corrupt media sequence space) */
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us, true, false, 0, 0));

  /* RTX queue has packets -> cannot probe (HOL priority protects playout) */
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us, true, true, 1, 0));

  /* Media queue has backlog -> cannot probe */
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us, true, true, 0, 1));

  /* Stale feedback (> 500ms) -> cannot probe */
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us + 600000LL, true, true, 0, 0));

  /* Overuse detected -> cannot probe */
  gcc.trendline.usage_state = GCC_BWE_OVERUSE;
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us, true, true, 0, 0));
  gcc.trendline.usage_state = GCC_BWE_NORMAL;

  /* Cooldown active -> cannot probe */
  pc.cooldown_until_us = now_us + 100000LL;
  assert(!sfu_probe_controller_should_probe(&pc, &gcc, now_us, true, true, 0, 0));
  assert(sfu_probe_controller_should_probe(&pc, &gcc, now_us + 100001LL, true, true, 0, 0));
}

static void test_target_calculation(void) {
  sfu_probe_controller_t pc;
  sfu_probe_controller_init(&pc);

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 300000u, 50000u, 5000000u);

  /* At 300 kbps: step is 500 kbps (min step), target is 800 kbps */
  sfu_probe_controller_start_probe(&pc, &gcc, 1000000LL);
  assert(pc.cluster_id == 1);
  assert(pc.state == SFU_PROBE_STATE_PROBING);
  assert(pc.base_bitrate_bps == 300000u);
  assert(pc.target_bitrate_bps == 800000u);
  assert(pc.bytes_target >= 750);
  assert(gcc.aimd.active_probing);

  sfu_probe_controller_reset(&pc);

  /* At 2 Mbps: step is 1 Mbps, target is 3 Mbps */
  gcc.aimd.current_bitrate_bps = 2000000u;
  sfu_probe_controller_start_probe(&pc, &gcc, 2000000LL);
  assert(pc.cluster_id == 2);
  assert(pc.target_bitrate_bps == 3000000u);

  sfu_probe_controller_reset(&pc);

  /* Clamped at max 5 Mbps: target is 5 Mbps */
  gcc.aimd.current_bitrate_bps = 4000000u;
  sfu_probe_controller_start_probe(&pc, &gcc, 3000000LL);
  assert(pc.target_bitrate_bps == 5000000u);

  sfu_probe_controller_reset(&pc);

  /* At or above max: start_probe returns false (nothing to prove) */
  gcc.aimd.current_bitrate_bps = 5000000u;
  assert(!sfu_probe_controller_start_probe(&pc, &gcc, 3000000LL));
  assert(pc.state == SFU_PROBE_STATE_IDLE);
}

static void test_successful_probe_cluster(void) {
  sfu_metrics_init();
  sfu_probe_controller_t pc;
  sfu_probe_controller_init(&pc);

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 1000000u, 50000u, 5000000u);

  int64_t now_us = 1000000LL;
  sfu_probe_controller_start_probe(&pc, &gcc, now_us);
  uint32_t cluster_id = pc.cluster_id;
  uint32_t target_bps = pc.target_bitrate_bps;
  assert(target_bps == 1500000u);

  /* Simulate transmitting 10 probe packets of 250 bytes over 15ms */
  for (int i = 0; i < 10; i++) {
    pc.bytes_sent += 250;
    pc.packets_sent++;
  }
  pc.state = SFU_PROBE_STATE_WAITING_FEEDBACK;
  pc.send_end_time_us = now_us + 15000LL;

  /* Still waiting feedback */
  uint32_t result_bps = 0;
  assert(!sfu_probe_controller_check_cluster_done(&pc, &gcc, now_us + 20000LL, &result_bps));

  /* Deliver 10 packets across 13.3ms: (2500 * 8 * 1000000) / 13333 = 1.5 Mbps */
  for (int i = 0; i < 10; i++) {
    int64_t recv_us = now_us + 50000LL + i * 1333LL;
    sfu_probe_controller_on_packet_received(&pc, cluster_id, 250, now_us + i * 1500LL, recv_us);
  }

  assert(sfu_probe_controller_check_cluster_done(&pc, &gcc, now_us + 70000LL, &result_bps));
  assert(result_bps == target_bps);
  assert(pc.state == SFU_PROBE_STATE_IDLE);
  assert(pc.clusters_succeeded == 1);
  assert(pc.cooldown_until_us == now_us + 70000LL + SFU_PROBE_COOLDOWN_SUCCESS_US);
  assert(sfu_metric_get("congestion_probe_succeeded") == 1);
  /* active_probing must be cleared so passive recovery probes are not suppressed forever */
  assert(!gcc.aimd.active_probing);
}

static void test_probe_abort_on_overuse(void) {
  sfu_probe_controller_t pc;
  sfu_probe_controller_init(&pc);

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 1000000u, 50000u, 5000000u);

  int64_t now_us = 1000000LL;
  sfu_probe_controller_start_probe(&pc, &gcc, now_us);
  pc.bytes_sent = 2000;
  pc.packets_sent = 8;
  pc.state = SFU_PROBE_STATE_WAITING_FEEDBACK;
  pc.send_end_time_us = now_us + 15000LL;

  /* GCC enters overuse */
  gcc.trendline.usage_state = GCC_BWE_OVERUSE;

  uint32_t result_bps = 0;
  assert(sfu_probe_controller_check_cluster_done(&pc, &gcc, now_us + 30000LL, &result_bps));
  assert(result_bps == 0);
  assert(pc.clusters_failed == 1);
  assert(pc.cooldown_until_us == now_us + 30000LL + SFU_PROBE_COOLDOWN_FAILURE_US);
  /* active_probing must be cleared on overuse so passive recovery is not blocked */
  assert(!gcc.aimd.active_probing);
}

static void test_probe_abort_on_packet_loss(void) {
  sfu_probe_controller_t pc;
  sfu_probe_controller_init(&pc);

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 1000000u, 50000u, 5000000u);

  int64_t now_us = 1000000LL;
  sfu_probe_controller_start_probe(&pc, &gcc, now_us);
  uint32_t cluster_id = pc.cluster_id;
  pc.bytes_sent = 2500;
  pc.packets_sent = 10;
  pc.state = SFU_PROBE_STATE_WAITING_FEEDBACK;
  pc.send_end_time_us = now_us + 15000LL;

  /* 4 packets lost out of 10 */
  for (int i = 0; i < 4; i++) {
    sfu_probe_controller_on_packet_lost(&pc, cluster_id);
  }
  for (int i = 0; i < 6; i++) {
    sfu_probe_controller_on_packet_received(&pc, cluster_id, 250, now_us + i * 1000LL, now_us + 50000LL + i * 1000LL);
  }

  uint32_t result_bps = 0;
  assert(sfu_probe_controller_check_cluster_done(&pc, &gcc, now_us + 60000LL, &result_bps));
  assert(result_bps == 0);
  assert(pc.clusters_failed == 1);
}

static void test_probe_explicit_abort(void) {
  sfu_probe_controller_t pc;
  sfu_probe_controller_init(&pc);

  gcc_bwe_context_t gcc;
  gcc_bwe_init(&gcc, 1000000u, 50000u, 5000000u);

  int64_t now_us = 1000000LL;
  sfu_probe_controller_start_probe(&pc, &gcc, now_us);
  assert(gcc.aimd.active_probing);

  sfu_probe_controller_abort(&pc, &gcc, now_us + 5000LL, "backlog");
  assert(pc.state == SFU_PROBE_STATE_IDLE);
  assert(pc.aborted);
  assert(!gcc.aimd.active_probing);
  assert(pc.clusters_aborted == 1);
  assert(pc.cooldown_until_us == now_us + 5000LL + SFU_PROBE_COOLDOWN_FAILURE_US);
}

int main(void) {
  test_lifecycle_and_reset();
  test_eligibility_and_conditions();
  test_target_calculation();
  test_successful_probe_cluster();
  test_probe_abort_on_overuse();
  test_probe_abort_on_packet_loss();
  test_probe_explicit_abort();
  printf("test_probe_controller: OK\n");
  return 0;
}
