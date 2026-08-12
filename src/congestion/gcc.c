#include "gcc.h"
#include <math.h>
#include <string.h>

#define GCC_BURST_GROUPING_THRESHOLD_US 5000LL

#define GCC_DELAY_SMOOTHING_ALPHA 0.9

#define GCC_OVERUSE_TIME_THRESHOLD_MS 10.0
#define GCC_THRESHOLD_ADAPT_K_UP 0.0087
#define GCC_THRESHOLD_ADAPT_K_DOWN 0.039
#define GCC_THRESHOLD_MAX_DELTA_MS 20.0
#define GCC_THRESHOLD_MIN_MS 6.0
#define GCC_THRESHOLD_MAX_MS 600.0
#define GCC_OVERUSE_GAP_DECAY_MS 75.0

#define GCC_AIMD_MIN_INCREASE_INTERVAL_US 100000LL
#define GCC_AIMD_ADDITIVE_BPS_PER_S 400000.0
#define GCC_AIMD_PROBE_HEADROOM_NUM 3
#define GCC_AIMD_PROBE_HEADROOM_DEN 2
#define GCC_AIMD_DECREASE_FACTOR_NUM 85
#define GCC_AIMD_DECREASE_FACTOR_DEN 100
#define GCC_ACK_WINDOW_US 150000LL
#define GCC_ACK_GAP_RESET_US 300000LL
#define GCC_ACK_EWMA_OLD_NUM 9
#define GCC_ACK_EWMA_DEN 10

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

void gcc_bwe_init(gcc_bwe_context_t *ctx, uint32_t start_bitrate, uint32_t min_bitrate, uint32_t max_bitrate) {
  memset(ctx, 0, sizeof(gcc_bwe_context_t));

  if (min_bitrate == 0 || max_bitrate == 0 || min_bitrate > max_bitrate || start_bitrate < min_bitrate || start_bitrate > max_bitrate) {
    min_bitrate = 50000;
    max_bitrate = 5000000;
    start_bitrate = 300000;
  }

  ctx->aimd.current_bitrate_bps = start_bitrate;
  ctx->aimd.min_bitrate_bps = min_bitrate;
  ctx->aimd.max_bitrate_bps = max_bitrate;
  ctx->aimd.state = GCC_RATE_CTRL_INCREASE;
  ctx->trendline.threshold_ms = 12.5;
  ctx->trendline.usage_state = GCC_BWE_NORMAL;
}

static double trendline_modified_trend(const gcc_trendline_estimator_t *te) {
  if (te->history_count < 2) {
    return 0.0;
  }
  double sum_x = 0, sum_y = 0;
  double x_min = te->arrival_time_ms[0], x_max = te->arrival_time_ms[0];
  for (int i = 0; i < te->history_count; i++) {
    sum_x += te->arrival_time_ms[i];
    sum_y += te->smoothed_delay_history_ms[i];
    if (te->arrival_time_ms[i] < x_min) {
      x_min = te->arrival_time_ms[i];
    }
    if (te->arrival_time_ms[i] > x_max) {
      x_max = te->arrival_time_ms[i];
    }
  }
  double avg_x = sum_x / te->history_count;
  double avg_y = sum_y / te->history_count;

  double numerator = 0, denominator = 0;
  for (int i = 0; i < te->history_count; i++) {
    double dx = te->arrival_time_ms[i] - avg_x;
    numerator += dx * (te->smoothed_delay_history_ms[i] - avg_y);
    denominator += dx * dx;
  }
  double slope = (denominator != 0.0) ? (numerator / denominator) : 0.0;
  return slope * (x_max - x_min);
}

static void trendline_adapt_threshold(gcc_trendline_estimator_t *te, double modified_trend, int64_t now_us) {
  if (te->last_threshold_update_us == 0) {
    te->last_threshold_update_us = now_us;
    return;
  }
  double delta_ms = (double)(now_us - te->last_threshold_update_us) / 1000.0;
  if (delta_ms <= 0) {
    return;
  }
  double abs_trend = fabs(modified_trend);
  if (abs_trend > te->threshold_ms + GCC_THRESHOLD_MAX_DELTA_MS) {
    te->last_threshold_update_us = now_us;
    return;
  }
  double k = (abs_trend < te->threshold_ms) ? GCC_THRESHOLD_ADAPT_K_DOWN : GCC_THRESHOLD_ADAPT_K_UP;
  double next = te->threshold_ms + k * (abs_trend - te->threshold_ms) * delta_ms;
  te->threshold_ms = clampd(next, GCC_THRESHOLD_MIN_MS, GCC_THRESHOLD_MAX_MS);
  te->last_threshold_update_us = now_us;
}

static void trendline_detect(gcc_trendline_estimator_t *te, double modified_trend, int64_t now_us) {
  if (te->last_overuse_check_us == 0) {
    te->last_overuse_check_us = now_us;
  }
  double gap_ms = (double)(now_us - te->last_overuse_check_us) / 1000.0;
  te->last_overuse_check_us = now_us;

  if (modified_trend > te->threshold_ms) {
    if (gap_ms > GCC_OVERUSE_GAP_DECAY_MS) {
      te->time_over_using_ms = 0;
    } else {
      te->time_over_using_ms += gap_ms;
    }
    if (te->time_over_using_ms >= GCC_OVERUSE_TIME_THRESHOLD_MS && te->history_count >= 2) {
      te->time_over_using_ms = 0;
      te->usage_state = GCC_BWE_OVERUSE;
    }
  } else if (modified_trend < -te->threshold_ms) {
    te->time_over_using_ms = 0;
    te->usage_state = GCC_BWE_UNDERUSE;
  } else {
    te->time_over_using_ms = 0;
    te->usage_state = GCC_BWE_NORMAL;
  }
}

static void update_trendline(gcc_trendline_estimator_t *te, double delay_variation_ms, int64_t arrival_time_us) {
  if (!te->have_first_arrival) {
    te->first_arrival_time_us = arrival_time_us;
    te->have_first_arrival = true;
  }

  te->accumulated_delay_ms += delay_variation_ms;
  te->smoothed_delay_ms = GCC_DELAY_SMOOTHING_ALPHA * te->smoothed_delay_ms + (1.0 - GCC_DELAY_SMOOTHING_ALPHA) * te->accumulated_delay_ms;

  double rel_arrival_ms = (double)(arrival_time_us - te->first_arrival_time_us) / 1000.0;

  te->arrival_time_ms[te->history_index] = rel_arrival_ms;
  te->smoothed_delay_history_ms[te->history_index] = te->smoothed_delay_ms;
  te->history_index = (te->history_index + 1) % GCC_TRENDLINE_WINDOW_SIZE;
  if (te->history_count < GCC_TRENDLINE_WINDOW_SIZE) {
    te->history_count++;
  }

  double modified_trend = trendline_modified_trend(te);
  te->trendline_slope = te->history_count > 0 ? modified_trend / te->history_count : 0.0;
  trendline_adapt_threshold(te, modified_trend, arrival_time_us);
  trendline_detect(te, modified_trend, arrival_time_us);
}

static void update_aimd(gcc_aimd_controller_t *aimd, gcc_bwe_usage_t usage, int64_t now_us) {
  switch (usage) {
    case GCC_BWE_OVERUSE: {
      uint64_t basis = aimd->current_bitrate_bps;
      if (aimd->have_ack_bitrate && aimd->ack_bitrate_bps < basis) {
        basis = aimd->ack_bitrate_bps;
      }
      uint64_t next = basis * GCC_AIMD_DECREASE_FACTOR_NUM / GCC_AIMD_DECREASE_FACTOR_DEN;
      aimd->current_bitrate_bps = (uint32_t)(next > UINT32_MAX ? UINT32_MAX : next);
      aimd->state = GCC_RATE_CTRL_DECREASE;
      break;
    }
    case GCC_BWE_UNDERUSE:
      aimd->state = GCC_RATE_CTRL_HOLD;
      break;
    case GCC_BWE_NORMAL:
      if (aimd->state == GCC_RATE_CTRL_DECREASE) {
        aimd->state = GCC_RATE_CTRL_HOLD;
        break;
      }
      aimd->state = GCC_RATE_CTRL_INCREASE;
      if (aimd->last_increase_us == 0) {
        aimd->last_increase_us = now_us;
      } else if (now_us - aimd->last_increase_us >= GCC_AIMD_MIN_INCREASE_INTERVAL_US) {
        double elapsed_s = (double)(now_us - aimd->last_increase_us) / 1000000.0;
        uint64_t add = (uint64_t)(GCC_AIMD_ADDITIVE_BPS_PER_S * elapsed_s);
        if (add == 0) {
          add = 1000;
        }
        uint64_t cap =
            aimd->have_ack_bitrate ? (uint64_t)aimd->ack_bitrate_bps * GCC_AIMD_PROBE_HEADROOM_NUM / GCC_AIMD_PROBE_HEADROOM_DEN : (uint64_t)UINT32_MAX;
        if (cap > aimd->current_bitrate_bps) {
          uint64_t next = (uint64_t)aimd->current_bitrate_bps + add;
          if (next > cap) {
            next = cap;
          }
          aimd->current_bitrate_bps = (uint32_t)(next > UINT32_MAX ? UINT32_MAX : next);
        }
        aimd->last_increase_us = now_us;
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

static void update_ack_bitrate(gcc_aimd_controller_t *aimd, const gcc_packet_info_t *pkt) {
  if (pkt->size_bytes == 0) {
    return;
  }

  if (aimd->ack_window_bytes > 0 && pkt->receive_time_us > aimd->ack_window_max_recv_us &&
      pkt->receive_time_us - aimd->ack_window_max_recv_us > GCC_ACK_GAP_RESET_US) {
    aimd->ack_window_bytes = 0;
    aimd->ack_window_min_recv_us = 0;
    aimd->ack_window_max_recv_us = 0;
    aimd->ack_bitrate_bps = 0;
    aimd->have_ack_bitrate = false;
  }

  if (aimd->ack_window_bytes == 0) {
    aimd->ack_window_min_recv_us = pkt->receive_time_us;
    aimd->ack_window_max_recv_us = pkt->receive_time_us;
  } else {
    if (pkt->receive_time_us < aimd->ack_window_min_recv_us) {
      aimd->ack_window_min_recv_us = pkt->receive_time_us;
    }
    if (pkt->receive_time_us > aimd->ack_window_max_recv_us) {
      aimd->ack_window_max_recv_us = pkt->receive_time_us;
    }
  }
  aimd->ack_window_bytes += pkt->size_bytes;

  int64_t span_us = aimd->ack_window_max_recv_us - aimd->ack_window_min_recv_us;
  if (span_us < GCC_ACK_WINDOW_US) {
    return;
  }

  uint64_t sample = aimd->ack_window_bytes * 8ull * 1000000ull / (uint64_t)span_us;
  if (sample > UINT32_MAX) {
    sample = UINT32_MAX;
  }
  if (aimd->have_ack_bitrate) {
    sample = ((uint64_t)aimd->ack_bitrate_bps * GCC_ACK_EWMA_OLD_NUM + sample) / GCC_ACK_EWMA_DEN;
  }
  aimd->ack_bitrate_bps = (uint32_t)sample;
  aimd->have_ack_bitrate = true;
  aimd->ack_window_bytes = 0;
  aimd->ack_window_min_recv_us = 0;
  aimd->ack_window_max_recv_us = 0;
}

void gcc_bwe_report_loss(gcc_bwe_context_t *ctx, uint32_t lost, uint32_t total) {
  if (!ctx || total == 0 || lost == 0) {
    return;
  }
  gcc_aimd_controller_t *aimd = &ctx->aimd;

  if (lost * 100 > total * 10) {
    if (aimd->have_ack_bitrate && aimd->current_bitrate_bps > aimd->ack_bitrate_bps) {
      aimd->current_bitrate_bps = aimd->ack_bitrate_bps;
      if (aimd->current_bitrate_bps < aimd->min_bitrate_bps) {
        aimd->current_bitrate_bps = aimd->min_bitrate_bps;
      }
    }
    aimd->state = GCC_RATE_CTRL_HOLD;
  } else if (lost * 100 > total * 2) {
    aimd->state = GCC_RATE_CTRL_HOLD;
  }
}

uint32_t gcc_bwe_process_twcc_packet(gcc_bwe_context_t *ctx, const gcc_packet_info_t *pkt) {
  gcc_arrival_group_t *cg = &ctx->current_group;

  bool reordered = cg->packet_count > 0 && pkt->send_time_us < cg->first_send_time_us;
  if (reordered) {
    return ctx->aimd.current_bitrate_bps;
  }

  update_ack_bitrate(&ctx->aimd, pkt);

  if (cg->packet_count == 0 || (pkt->send_time_us - cg->first_send_time_us) <= GCC_BURST_GROUPING_THRESHOLD_US) {
    if (cg->packet_count == 0) {
      cg->first_send_time_us = pkt->send_time_us;
      cg->first_recv_time_us = pkt->receive_time_us;
    }
    cg->last_send_time_us = pkt->send_time_us;
    cg->last_recv_time_us = pkt->receive_time_us;
    cg->total_size += pkt->size_bytes;
    cg->packet_count++;
    return ctx->aimd.current_bitrate_bps;
  }

  if (ctx->prev_group.packet_count > 0) {
    double delta_send_ms = (double)(cg->last_send_time_us - ctx->prev_group.last_send_time_us) / 1000.0;
    double delta_recv_ms = (double)(cg->last_recv_time_us - ctx->prev_group.last_recv_time_us) / 1000.0;
    double delay_variation_ms = delta_recv_ms - delta_send_ms;

    update_trendline(&ctx->trendline, delay_variation_ms, cg->last_recv_time_us);
    update_aimd(&ctx->aimd, ctx->trendline.usage_state, cg->last_recv_time_us);
  }

  ctx->prev_group = *cg;
  memset(cg, 0, sizeof(gcc_arrival_group_t));
  cg->first_send_time_us = pkt->send_time_us;
  cg->first_recv_time_us = pkt->receive_time_us;
  cg->last_send_time_us = pkt->send_time_us;
  cg->last_recv_time_us = pkt->receive_time_us;
  cg->total_size = pkt->size_bytes;
  cg->packet_count = 1;

  return ctx->aimd.current_bitrate_bps;
}
