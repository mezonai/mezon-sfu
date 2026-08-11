#ifndef SFU_GCC_H
#define SFU_GCC_H

#include <stdbool.h>
#include <stdint.h>

#define GCC_TRENDLINE_WINDOW_SIZE 20

// State of the overuse detector
typedef enum { GCC_BWE_NORMAL = 0, GCC_BWE_UNDERUSE, GCC_BWE_OVERUSE } gcc_bwe_usage_t;

// State of the AIMD rate controller
typedef enum { GCC_RATE_CTRL_HOLD = 0, GCC_RATE_CTRL_INCREASE, GCC_RATE_CTRL_DECREASE } gcc_rate_ctrl_state_t;

typedef struct gcc_packet_info {
  uint16_t sequence_number;
  int64_t send_time_us;
  int64_t receive_time_us;
  uint32_t size_bytes;
} gcc_packet_info_t;

typedef struct gcc_arrival_group {
  int64_t first_send_time_us;
  int64_t last_send_time_us;
  int64_t first_recv_time_us;
  int64_t last_recv_time_us;
  uint32_t total_size;
  int packet_count;
} gcc_arrival_group_t;

typedef struct gcc_trendline_estimator {
  double arrival_time_ms[GCC_TRENDLINE_WINDOW_SIZE];
  double smoothed_delay_history_ms[GCC_TRENDLINE_WINDOW_SIZE];
  int history_index;
  int history_count;

  double accumulated_delay_ms;
  double smoothed_delay_ms;
  double trendline_slope;

  double threshold_ms;
  int64_t last_threshold_update_us;

  double time_over_using_ms;
  int64_t last_overuse_check_us;

  int64_t first_arrival_time_us;
  bool have_first_arrival;

  gcc_bwe_usage_t usage_state;
} gcc_trendline_estimator_t;

typedef struct gcc_aimd_controller {
  uint32_t current_bitrate_bps;
  uint32_t min_bitrate_bps;
  uint32_t max_bitrate_bps;

  gcc_rate_ctrl_state_t state;

  int64_t last_increase_us;
  uint32_t ack_bitrate_bps;
  uint64_t ack_window_bytes;
  int64_t ack_window_min_recv_us;
  int64_t ack_window_max_recv_us;
  bool have_ack_bitrate;
} gcc_aimd_controller_t;

typedef struct gcc_bwe_context {
  gcc_arrival_group_t current_group;
  gcc_arrival_group_t prev_group;
  gcc_trendline_estimator_t trendline;
  gcc_aimd_controller_t aimd;
} gcc_bwe_context_t;

void gcc_bwe_init(gcc_bwe_context_t *ctx, uint32_t start_bitrate, uint32_t min_bitrate, uint32_t max_bitrate);
uint32_t gcc_bwe_process_twcc_packet(gcc_bwe_context_t *ctx, const gcc_packet_info_t *pkt);
void gcc_bwe_report_loss(gcc_bwe_context_t *ctx, uint32_t lost, uint32_t total);

#endif  // SFU_GCC_H
