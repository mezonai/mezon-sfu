#ifndef SFU_GCC_H
#define SFU_GCC_H

#include <stdbool.h>
#include <stdint.h>

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
  double arrival_time_ms[GCC_TRENDLINE_WINDOW_SIZE];
  double smoothed_delay_ms[GCC_TRENDLINE_WINDOW_SIZE];
  int history_index;
  int history_count;

  double accumulated_delay;
  double smoothed_delay;
  double trendline_slope;

  double threshold;
  int64_t last_update_ms;
  double time_over_using;
  int overuse_counter;

  gcc_bwe_usage_t usage_state;
} gcc_trendline_estimator_t;

// The Additive Increase Multiplicative Decrease controller
typedef struct gcc_aimd_controller {
  uint32_t current_bitrate_bps;
  uint32_t min_bitrate_bps;
  uint32_t max_bitrate_bps;

  int64_t last_time_ms;
  gcc_rate_ctrl_state_t state;

  double avg_max_bitrate;
  double var_max_bitrate;
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

#endif  // SFU_GCC_H
