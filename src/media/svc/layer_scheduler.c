#include "media/svc/layer_scheduler.h"

#include <stdatomic.h>
#include <string.h>

#include "peer/session.h"
#include "runtime/timer.h"
#include "util/log.h"

void sfu_layer_scheduler_init(sfu_layer_scheduler_t *sched, uint32_t initial_publisher) {
  memset(sched, 0, sizeof(*sched));
  sched->active_publisher_id = initial_publisher;
  sched->needs_keyframe = true;
  sched->target_sid = 0;
  sched->target_tid = 2;
}

sfu_layer_scheduler_t *sfu_layer_scheduler_for_stream(sfu_peer_session_t *session, uint32_t publisher_id, sfu_media_kind_t source) {
  if (!session || !sfu_session_video_runtime_ready(session) || !session->egress.schedulers || publisher_id == 0 || source == SFU_MEDIA_AUDIO) {
    return NULL;
  }
  uint64_t stream_key = ((uint64_t)publisher_id << 8) | (uint8_t)source;

  sfu_layer_scheduler_slot_t *free_slot = NULL;
  for (uint32_t i = 0; i < SFU_LAYER_SCHEDULER_CAP; i++) {
    sfu_layer_scheduler_slot_t *slot = &session->egress.schedulers[i];
    if (slot->publisher_id == stream_key) {
      return &slot->sched;
    }
    if (!free_slot && slot->publisher_id == 0) {
      free_slot = slot;
    }
  }

  if (!free_slot) {
    SFU_LOG_WARN("session %u: scheduler table full (%d streams); cannot track publisher %u source %u", session->peer_id, SFU_LAYER_SCHEDULER_CAP,
                 publisher_id, (unsigned)source);
    return NULL;
  }

  free_slot->publisher_id = stream_key;
  sfu_layer_scheduler_init(&free_slot->sched, publisher_id);
  return &free_slot->sched;
}

sfu_layer_scheduler_t *sfu_layer_scheduler_for(sfu_peer_session_t *session, uint32_t publisher_id) {
  return sfu_layer_scheduler_for_stream(session, publisher_id, SFU_MEDIA_VIDEO);
}

static void layer_scheduler_begin_picture(sfu_layer_scheduler_t *sched, uint32_t rtp_timestamp) {
  if (sched->picture_valid && sched->picture_timestamp == rtp_timestamp) {
    return;
  }

  if (sched->keyframe_active) {
    sched->needs_keyframe = true;
    sched->keyframe_active = false;
    sched->keyframe_failed = true;
  }
  sched->transition_active = false;
  sched->transition_failed = false;
  sched->temporal_transition_active = false;
  sched->temporal_transition_failed = false;
  sched->pacer_frame_active = false;

  sched->picture_valid = true;
  sched->picture_timestamp = rtp_timestamp;
  sched->started_sid_mask = 0;
  sched->completed_sid_mask = 0;
  sched->failed_sid_mask = 0;
  sched->transition_sid = 0;
  sched->transition_timestamp = 0;
  sched->temporal_transition_tid = 0;
  sched->temporal_transition_timestamp = 0;
}

sfu_pacer_class_t sfu_layer_scheduler_classify_frame(const sfu_layer_scheduler_t *sched, const sfu_svc_descriptor_t *desc) {
  (void)sched;
  if (desc->sid > 0 || desc->tid > 0) {
    return SFU_PACER_CLASS_VIDEO_ENH;
  }
  return SFU_PACER_CLASS_VIDEO_BASE;
}

bool sfu_layer_scheduler_prepare_packet(sfu_layer_scheduler_t *sched, const sfu_svc_descriptor_t *desc, bool is_keyframe,
                                        sfu_layer_scheduler_decision_t *decision) {
  if (!sched || !desc || !decision) {
    return false;
  }

  memset(decision, 0, sizeof(*decision));
  decision->rtp_timestamp = desc->rtp_timestamp;
  decision->sid = desc->sid;
  decision->tid = desc->tid;
  decision->b_bit = desc->b_bit;
  decision->e_bit = desc->e_bit;
  decision->pacer_class = sfu_layer_scheduler_classify_frame(sched, desc);
  decision->pacer_frame_continuation = decision->pacer_class == SFU_PACER_CLASS_VIDEO_ENH && desc->b_bit == 0;

  if (desc->sid >= 8) {
    return false;
  }

  layer_scheduler_begin_picture(sched, desc->rtp_timestamp);

  if (sched->target_sid < sched->current_sid) {
    sched->current_sid = sched->target_sid;
  }
  if (sched->target_tid < sched->current_tid) {
    sched->current_tid = sched->target_tid;
  }

  uint8_t sid_mask = (uint8_t)(1u << desc->sid);
  if ((sched->failed_sid_mask & sid_mask) != 0 || desc->sid > sched->target_sid || desc->tid > sched->target_tid) {
    return false;
  }
  if (desc->b_bit == 0 && (sched->started_sid_mask & sid_mask) == 0) {
    return false;
  }

  if (sched->needs_keyframe) {
    if (!sched->keyframe_active) {
      if (!is_keyframe || desc->b_bit == 0 || desc->sid != 0) {
        return false;
      }
      decision->start_keyframe = true;
    } else if (sched->keyframe_failed || sched->keyframe_timestamp != desc->rtp_timestamp || desc->sid != 0 || desc->p_bit != 0) {
      return false;
    }
    decision->keyframe_packet = true;
    decision->transition_packet = true;
  }

  if (desc->sid > sched->current_sid) {
    uint8_t candidate_sid = (uint8_t)(sched->current_sid + 1);
    if (desc->sid != candidate_sid || candidate_sid > sched->target_sid) {
      return false;
    }

    if (sched->transition_active) {
      if (sched->transition_timestamp != desc->rtp_timestamp || sched->transition_sid != desc->sid || sched->transition_failed) {
        return false;
      }
    } else {
      if (desc->b_bit == 0 || desc->p_bit != 0) {
        return false;
      }
      uint8_t lower_mask = (uint8_t)(1u << sched->current_sid);
      if (desc->d_bit != 0 && ((sched->completed_sid_mask & lower_mask) == 0 || (sched->failed_sid_mask & lower_mask) != 0)) {
        return false;
      }
      decision->start_transition = true;
    }
    decision->transition_packet = true;
  }

  if (desc->tid > sched->current_tid) {
    if (sched->temporal_transition_active) {
      if (sched->temporal_transition_timestamp != desc->rtp_timestamp || sched->temporal_transition_tid != desc->tid || sched->temporal_transition_failed) {
        return false;
      }
    } else {
      if (desc->b_bit == 0 || desc->u_bit == 0) {
        return false;
      }
      decision->start_temporal_transition = true;
    }
    decision->temporal_transition_packet = true;
    decision->transition_packet = true;
  }

  if (desc->b_bit == 0 && (sched->started_sid_mask & sid_mask) != 0) {
    decision->transition_packet = true;
  }
  if (sched->target_sid > sched->current_sid && desc->p_bit == 0) {
    decision->transition_packet = true;
  }
  bool admitted_enh_continuation = desc->b_bit == 0 && sched->pacer_frame_active && sched->pacer_frame_timestamp == desc->rtp_timestamp &&
                                       sched->pacer_frame_sid == desc->sid && sched->pacer_frame_tid == desc->tid;
  if (decision->transition_packet && !admitted_enh_continuation) {
    decision->pacer_class = SFU_PACER_CLASS_VIDEO_TRANSITION;
    decision->pacer_frame_continuation = false;
  }

  if (decision->pacer_class == SFU_PACER_CLASS_VIDEO_ENH) {
    decision->pacer_frame_start = desc->b_bit != 0;
    decision->pacer_frame_end = desc->e_bit != 0;
    if (decision->pacer_frame_start) {
      if (sched->pacer_frame_active) {
        return false;
      }
    } else {
      decision->pacer_frame_continuation = true;
      if (!sched->pacer_frame_active || sched->pacer_frame_timestamp != desc->rtp_timestamp || sched->pacer_frame_sid != desc->sid ||
          sched->pacer_frame_tid != desc->tid) {
        return false;
      }
    }
  }

  if (desc->l_bit == 0) {
    decision->set_marker = desc->e_bit != 0;
  } else {
    uint8_t output_sid = sched->current_sid;
    if (sched->transition_active && !sched->transition_failed && sched->transition_timestamp == desc->rtp_timestamp) {
      output_sid = sched->transition_sid;
    } else if (decision->start_transition) {
      output_sid = desc->sid;
    } else if (sched->target_sid > sched->current_sid && desc->p_bit == 0) {
      output_sid = sched->target_sid;
    }
    decision->set_marker = desc->e_bit != 0 && desc->sid == output_sid;
  }
  decision->should_forward = true;
  return true;
}

void sfu_layer_scheduler_commit_packet(sfu_layer_scheduler_t *sched, const sfu_layer_scheduler_decision_t *decision) {
  if (!sched || !decision || !decision->should_forward || !sched->picture_valid || sched->picture_timestamp != decision->rtp_timestamp || decision->sid >= 8) {
    return;
  }

  uint8_t sid_mask = (uint8_t)(1u << decision->sid);
  if (decision->b_bit != 0) {
    sched->started_sid_mask |= sid_mask;
  }

  if (decision->start_keyframe) {
    sched->keyframe_active = true;
    sched->keyframe_failed = false;
    sched->keyframe_timestamp = decision->rtp_timestamp;
  }
  if (decision->start_transition) {
    sched->transition_active = true;
    sched->transition_failed = false;
    sched->transition_sid = decision->sid;
    sched->transition_timestamp = decision->rtp_timestamp;
  }
  if (decision->start_temporal_transition) {
    sched->temporal_transition_active = true;
    sched->temporal_transition_failed = false;
    sched->temporal_transition_tid = decision->tid;
    sched->temporal_transition_timestamp = decision->rtp_timestamp;
  }

  if (decision->pacer_frame_start && !decision->pacer_frame_end) {
    sched->pacer_frame_active = true;
    sched->pacer_frame_timestamp = decision->rtp_timestamp;
    sched->pacer_frame_sid = decision->sid;
    sched->pacer_frame_tid = decision->tid;
  } else if (decision->pacer_frame_end && sched->pacer_frame_active && sched->pacer_frame_timestamp == decision->rtp_timestamp &&
             sched->pacer_frame_sid == decision->sid && sched->pacer_frame_tid == decision->tid) {
    sched->pacer_frame_active = false;
  }

  if (decision->e_bit != 0 && (sched->failed_sid_mask & sid_mask) == 0) {
    sched->completed_sid_mask |= sid_mask;
  }

  if (decision->keyframe_packet && decision->e_bit != 0) {
    bool complete =
        sched->keyframe_active && !sched->keyframe_failed && sched->keyframe_timestamp == decision->rtp_timestamp && (sched->failed_sid_mask & sid_mask) == 0;
    sched->needs_keyframe = !complete;
    sched->keyframe_active = false;
    sched->keyframe_failed = !complete;
  }

  if (sched->transition_active && sched->transition_timestamp == decision->rtp_timestamp && sched->transition_sid == decision->sid && decision->e_bit != 0) {
    if (!sched->transition_failed && (sched->failed_sid_mask & sid_mask) == 0 && decision->sid <= sched->target_sid) {
      sched->current_sid = decision->sid;
    }
    sched->transition_active = false;
    sched->transition_failed = false;
  }

  if (sched->temporal_transition_active && sched->temporal_transition_timestamp == decision->rtp_timestamp && sched->temporal_transition_tid == decision->tid &&
      decision->e_bit != 0) {
    if (!sched->temporal_transition_failed && (sched->failed_sid_mask & sid_mask) == 0 && decision->tid <= sched->target_tid) {
      sched->current_tid = decision->tid;
    }
    sched->temporal_transition_active = false;
    sched->temporal_transition_failed = false;
  }
}

void sfu_layer_scheduler_reject_packet(sfu_layer_scheduler_t *sched, const sfu_layer_scheduler_decision_t *decision) {
  if (!sched || !decision || !sched->picture_valid || sched->picture_timestamp != decision->rtp_timestamp || decision->sid >= 8) {
    return;
  }

  sched->failed_sid_mask |= (uint8_t)(1u << decision->sid);
  if (sched->pacer_frame_active && sched->pacer_frame_timestamp == decision->rtp_timestamp && sched->pacer_frame_sid == decision->sid &&
      sched->pacer_frame_tid == decision->tid) {
    sched->pacer_frame_active = false;
  }
  if (decision->keyframe_packet || (sched->keyframe_active && sched->keyframe_timestamp == decision->rtp_timestamp && decision->sid == 0)) {
    sched->needs_keyframe = true;
    sched->keyframe_active = false;
    sched->keyframe_failed = true;
  }
  if ((decision->start_transition || sched->transition_active) && sched->transition_timestamp == decision->rtp_timestamp &&
      sched->transition_sid == decision->sid) {
    sched->transition_failed = true;
  }
  if ((decision->start_temporal_transition || sched->temporal_transition_active) && sched->temporal_transition_timestamp == decision->rtp_timestamp &&
      sched->temporal_transition_tid == decision->tid) {
    sched->temporal_transition_failed = true;
  }
}

typedef struct sfu_layer_rung {
  uint32_t rate_bps;
  uint8_t sid;
  uint8_t tid;
} sfu_layer_rung_t;

static const sfu_layer_rung_t k_layer_ladder[] = {
    {200000, 0, 0},  /* L1T3 base temporal layer */
    {600000, 0, 1},  /* L1T3 middle temporal layer */
    {1200000, 0, 2}, /* L1T3 full temporal stream */
};
#define SFU_LAYER_LADDER_LEN (sizeof(k_layer_ladder) / sizeof(k_layer_ladder[0]))
#define SFU_LAYER_UP_HEADROOM_NUM 6 /* up threshold = rate * 1.2 */
#define SFU_LAYER_UP_HEADROOM_DEN 5
#define SFU_LAYER_DWELL_US 500000LL

void sfu_layer_scheduler_set_bitrate(sfu_layer_scheduler_t *sched, uint32_t bitrate_bps) {
  uint8_t target_sid = 0, target_tid = 0;
  int chosen = -1;

  for (int i = (int)SFU_LAYER_LADDER_LEN - 1; i >= 0; i--) {
    uint64_t up = (uint64_t)k_layer_ladder[i].rate_bps * SFU_LAYER_UP_HEADROOM_NUM / SFU_LAYER_UP_HEADROOM_DEN;
    if (bitrate_bps >= up) {
      chosen = i;
      break;
    }
  }
  if (chosen >= 0) {
    target_sid = k_layer_ladder[chosen].sid;
    target_tid = k_layer_ladder[chosen].tid;
  }

  if (sched->last_target_change_us != 0 && chosen < (int)SFU_LAYER_LADDER_LEN - 1) {
    int current_rung = -1;
    for (int i = (int)SFU_LAYER_LADDER_LEN - 1; i >= 0; i--) {
      if (sched->target_sid >= k_layer_ladder[i].sid && sched->target_tid >= k_layer_ladder[i].tid) {
        current_rung = i;
        break;
      }
    }
    if (current_rung > chosen && current_rung >= 0 && bitrate_bps >= k_layer_ladder[current_rung].rate_bps) {
      chosen = current_rung;
      target_sid = k_layer_ladder[chosen].sid;
      target_tid = k_layer_ladder[chosen].tid;
    }
  }

  if (target_sid == sched->target_sid && target_tid == sched->target_tid) {
    return;
  }

  int64_t now = (int64_t)sfu_now_us();
  if (sched->last_target_change_us != 0 && now - sched->last_target_change_us < SFU_LAYER_DWELL_US) {
    return;
  }
  sched->last_target_change_us = now;
  sched->target_sid = target_sid;
  sched->target_tid = target_tid;
}

void sfu_layer_scheduler_switch_source(sfu_peer_session_t *session, uint32_t new_publisher_id) {
  sfu_layer_scheduler_t *sched = sfu_layer_scheduler_for(session, new_publisher_id);
  if (!sched) {
    return;
  }

  sched->active_publisher_id = new_publisher_id;
  sched->current_sid = 0;
  sched->current_tid = 0;
  sched->needs_keyframe = true;
  sched->picture_valid = false;
  sched->started_sid_mask = 0;
  sched->completed_sid_mask = 0;
  sched->failed_sid_mask = 0;
  sched->transition_active = false;
  sched->transition_failed = false;
  sched->temporal_transition_active = false;
  sched->temporal_transition_failed = false;
  sched->keyframe_active = false;
  sched->keyframe_failed = false;
  sched->pacer_frame_active = false;

  atomic_fetch_add_explicit(&session->egress.generation, 1, memory_order_acq_rel);
}
