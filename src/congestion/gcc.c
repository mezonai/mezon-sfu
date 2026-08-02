#include "gcc.h"
#include <math.h>
#include <string.h>

#define GCC_INTER_GROUP_DELAY_THRESHOLD_MS 5.0
#define GCC_ALPHA_SMOOTHING 0.9

void gcc_bwe_init(gcc_bwe_context_t *ctx, uint32_t start_bitrate, uint32_t min_bitrate, uint32_t max_bitrate) {
  memset(ctx, 0, sizeof(gcc_bwe_context_t));
  ctx->aimd.current_bitrate_bps = start_bitrate;
  ctx->aimd.min_bitrate_bps = min_bitrate;
  ctx->aimd.max_bitrate_bps = max_bitrate;
  ctx->aimd.state = GCC_RATE_CTRL_INCREASE;
  ctx->trendline.threshold = 12.5;  // Default adaptive threshold starting point
}

// Internal function to calculate linear regression (Trendline)
static void update_trendline(gcc_trendline_estimator_t *te, double delta_arrival, double delta_delay, int64_t now_ms) {
  te->accumulated_delay += delta_delay;
  te->smoothed_delay = GCC_ALPHA_SMOOTHING * te->smoothed_delay + (1.0 - GCC_ALPHA_SMOOTHING) * te->accumulated_delay;

  // Insert into ring buffer
  te->arrival_time_ms[te->history_index] = delta_arrival;
  te->smoothed_delay_ms[te->history_index] = te->smoothed_delay;
  te->history_index = (te->history_index + 1) % GCC_TRENDLINE_WINDOW_SIZE;
  if (te->history_count < GCC_TRENDLINE_WINDOW_SIZE) {
    te->history_count++;
  }

  if (te->history_count == GCC_TRENDLINE_WINDOW_SIZE) {
    double sum_x = 0, sum_y = 0;
    for (int i = 0; i < te->history_count; i++) {
      sum_x += te->arrival_time_ms[i];
      sum_y += te->smoothed_delay_ms[i];
    }
    double avg_x = sum_x / te->history_count;
    double avg_y = sum_y / te->history_count;

    double numerator = 0, denominator = 0;
    for (int i = 0; i < te->history_count; i++) {
      double diff_x = te->arrival_time_ms[i] - avg_x;
      double diff_y = te->smoothed_delay_ms[i] - avg_y;
      numerator += diff_x * diff_y;
      denominator += diff_x * diff_x;
    }

    te->trendline_slope = (denominator != 0) ? (numerator / denominator) : 0;

    // Overuse detection logic (simplified Pion logic)
    double modified_trend = te->trendline_slope * te->history_count;
    if (modified_trend > te->threshold) {
      te->time_over_using += (now_ms - te->last_update_ms);
      if (te->time_over_using > 10.0 && te->overuse_counter > 1) {
        te->usage_state = GCC_BWE_OVERUSE;
      }
      te->overuse_counter++;
    } else if (modified_trend < -te->threshold) {
      te->time_over_using = 0;
      te->overuse_counter = 0;
      te->usage_state = GCC_BWE_UNDERUSE;
    } else {
      te->time_over_using = 0;
      te->overuse_counter = 0;
      te->usage_state = GCC_BWE_NORMAL;
    }
  }
  te->last_update_ms = now_ms;
}

// Internal function to update AIMD based on Trendline state
static void update_aimd(gcc_aimd_controller_t *aimd, gcc_bwe_usage_t usage, int64_t now_ms) {
  switch (usage) {
    case GCC_BWE_OVERUSE:
      if (aimd->state != GCC_RATE_CTRL_DECREASE) {
        // Multiplicative Decrease (cut by ~15%)
        aimd->current_bitrate_bps = (uint32_t)(aimd->current_bitrate_bps * 0.85);
        aimd->state = GCC_RATE_CTRL_DECREASE;
      }
      break;
    case GCC_BWE_UNDERUSE:
      aimd->state = GCC_RATE_CTRL_HOLD;
      break;
    case GCC_BWE_NORMAL:
      if (aimd->state == GCC_RATE_CTRL_HOLD) {
        aimd->state = GCC_RATE_CTRL_INCREASE;
      }
      if (aimd->state == GCC_RATE_CTRL_INCREASE) {
        // Additive Increase (approx 8% of current per RTT, simplified here)
        uint32_t increase = (aimd->current_bitrate_bps * 8) / 100;
        aimd->current_bitrate_bps += increase;
      }
      break;
  }

  if (aimd->current_bitrate_bps < aimd->min_bitrate_bps) {
    aimd->current_bitrate_bps = aimd->min_bitrate_bps;
  }
  if (aimd->current_bitrate_bps > aimd->max_bitrate_bps) {
    aimd->current_bitrate_bps = aimd->max_bitrate_bps;
  }
}

uint32_t gcc_bwe_process_twcc_packet(gcc_bwe_context_t *ctx, const gcc_packet_info_t *pkt) {
  gcc_arrival_group_t *cg = &ctx->current_group;

  if (cg->packet_count == 0 || (pkt->send_time_ms - cg->first_send_time_ms) <= GCC_INTER_GROUP_DELAY_THRESHOLD_MS) {
    if (cg->packet_count == 0) {
      cg->first_send_time_ms = pkt->send_time_ms;
      cg->first_recv_time_ms = pkt->receive_time_ms;
    }
    cg->last_send_time_ms = pkt->send_time_ms;
    cg->last_recv_time_ms = pkt->receive_time_ms;
    cg->total_size += pkt->size_bytes;
    cg->packet_count++;
  } else {
    // Group complete, compare with previous group
    if (ctx->prev_group.packet_count > 0) {
      double delta_send = (double)(cg->last_send_time_ms - ctx->prev_group.last_send_time_ms);
      double delta_recv = (double)(cg->last_recv_time_ms - ctx->prev_group.last_recv_time_ms);
      double delay_gradient = delta_recv - delta_send;

      update_trendline(&ctx->trendline, delta_recv, delay_gradient, pkt->receive_time_ms);

      update_aimd(&ctx->aimd, ctx->trendline.usage_state, pkt->receive_time_ms);
    }

    ctx->prev_group = *cg;
    memset(cg, 0, sizeof(gcc_arrival_group_t));
    cg->first_send_time_ms = pkt->send_time_ms;
    cg->first_recv_time_ms = pkt->receive_time_ms;
    cg->last_send_time_ms = pkt->send_time_ms;
    cg->last_recv_time_ms = pkt->receive_time_ms;
    cg->total_size = pkt->size_bytes;
    cg->packet_count = 1;
  }

  return ctx->aimd.current_bitrate_bps;
}
