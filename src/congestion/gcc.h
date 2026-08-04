#ifndef SFU_GCC_H
#define SFU_GCC_H

#include <stdbool.h>
#include <stdint.h>

/* Delay-based congestion estimator (rewrite per the security review's GCC
 * audit — CC-07/CC-08). All times are integer microseconds, matching the
 * TWCC parser and send history.
 *
 * Pipeline per completed arrival group (draft-ietf-rmcat-gcc-02 shape):
 *   1. Burst grouping by send time (5 ms threshold).
 *   2. Delay variation between consecutive completed groups.
 *   3. Trendline regression of smoothed accumulated delay against CUMULATIVE
 *      relative arrival time (never per-group intervals — that was CC-07).
 *   4. Overuse detector with an adaptive threshold and gap-safe overuse
 *      timing.
 *   5. Time-paced AIMD: increases are rate-limited by elapsed time and
 *      capped against the acknowledged bitrate; decreases anchor to 85% of
 *      the acknowledged bitrate, never to the previous target. The state
 *      machine cannot freeze in DECREASE (that was CC-08). */

#define GCC_TRENDLINE_WINDOW_SIZE 20

// State of the overuse detector
typedef enum { GCC_BWE_NORMAL = 0, GCC_BWE_UNDERUSE, GCC_BWE_OVERUSE } gcc_bwe_usage_t;

// State of the AIMD rate controller
typedef enum { GCC_RATE_CTRL_HOLD = 0, GCC_RATE_CTRL_INCREASE, GCC_RATE_CTRL_DECREASE } gcc_rate_ctrl_state_t;

// Represents a packet parsed from TWCC feedback
typedef struct gcc_packet_info {
  uint16_t sequence_number;
  int64_t send_time_us;
  int64_t receive_time_us;
  uint32_t size_bytes;
} gcc_packet_info_t;

// Groups packets that arrived together (bursts)
typedef struct gcc_arrival_group {
  int64_t first_send_time_us;
  int64_t last_send_time_us;
  int64_t first_recv_time_us;
  int64_t last_recv_time_us;
  uint32_t total_size;
  int packet_count;
} gcc_arrival_group_t;

// The Trendline Estimator state
typedef struct gcc_trendline_estimator {
  /* Chronological ring of (relative arrival time, smoothed delay) samples.
   * arrival_time_ms is the group completion time minus the first ever
   * observed completion time (in ms, as a double), so the regression x-axis
   * is genuine elapsed time. */
  double arrival_time_ms[GCC_TRENDLINE_WINDOW_SIZE];
  double smoothed_delay_history_ms[GCC_TRENDLINE_WINDOW_SIZE];
  int history_index;
  int history_count;

  double accumulated_delay_ms;
  double smoothed_delay_ms;
  double trendline_slope;

  /* Adaptive threshold (ms of delay variation) and its last update time. */
  double threshold_ms;
  int64_t last_threshold_update_us;

  /* Overuse timing: wall time spent above threshold, gap-safe. */
  double time_over_using_ms;
  int64_t last_overuse_check_us;

  /* First group completion time (us); the x-axis origin. */
  int64_t first_arrival_time_us;
  bool have_first_arrival;

  gcc_bwe_usage_t usage_state;
} gcc_trendline_estimator_t;

// The Additive Increase Multiplicative Decrease controller
typedef struct gcc_aimd_controller {
  uint32_t current_bitrate_bps;
  uint32_t min_bitrate_bps;
  uint32_t max_bitrate_bps;

  gcc_rate_ctrl_state_t state;

  /* Last time the estimate was allowed to grow; paces additive increase. */
  int64_t last_increase_us;
  /* Acknowledged (measured receive) bitrate, updated per completed group. */
  uint32_t ack_bitrate_bps;
  bool have_ack_bitrate;
} gcc_aimd_controller_t;

// The main GCC context to attach to sfu_peer_session_t
typedef struct gcc_bwe_context {
  gcc_arrival_group_t current_group;
  gcc_arrival_group_t prev_group;
  gcc_trendline_estimator_t trendline;
  gcc_aimd_controller_t aimd;
} gcc_bwe_context_t;

void gcc_bwe_init(gcc_bwe_context_t *ctx, uint32_t start_bitrate, uint32_t min_bitrate, uint32_t max_bitrate);
uint32_t gcc_bwe_process_twcc_packet(gcc_bwe_context_t *ctx, const gcc_packet_info_t *pkt);

/* Applies a loss signal from a completed TWCC feedback batch (CC-13):
 * `lost` packets out of `total` reported statuses. Loss above 10% caps the
 * estimate at the acknowledged receive rate; loss above 2% blocks further
 * increases for this batch. Below 2% is a no-op. */
void gcc_bwe_report_loss(gcc_bwe_context_t *ctx, uint32_t lost, uint32_t total);

#endif  // SFU_GCC_H
