#ifndef SFU_MEDIA_SVC_LAYER_SCHEDULER_H
#define SFU_MEDIA_SVC_LAYER_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include "media/svc/svc_descriptor.h"
#include "sfu/datadef.h"

#define SFU_LAYER_SCHEDULER_CAP 16

typedef struct sfu_layer_scheduler {
  int64_t last_target_change_us;
  uint64_t active_publisher_id;
  uint32_t picture_timestamp;
  uint32_t transition_timestamp;
  uint32_t temporal_transition_timestamp;
  uint32_t keyframe_timestamp;
  uint8_t target_sid;
  uint8_t target_tid;
  uint8_t current_sid;
  uint8_t current_tid;
  uint8_t started_sid_mask;
  uint8_t completed_sid_mask;
  uint8_t failed_sid_mask;
  uint8_t transition_sid;
  uint8_t temporal_transition_tid;
  bool needs_keyframe;
  bool is_pinned;
  bool picture_valid;
  bool transition_active;
  bool transition_failed;
  bool temporal_transition_active;
  bool temporal_transition_failed;
  bool keyframe_active;
  bool keyframe_failed;
} sfu_layer_scheduler_t;

typedef struct sfu_layer_scheduler_decision {
  uint32_t rtp_timestamp;
  uint8_t sid;
  uint8_t tid;
  uint8_t b_bit;
  uint8_t e_bit;
  bool should_forward;
  bool set_marker;
  bool start_keyframe;
  bool keyframe_packet;
  bool start_transition;
  bool transition_packet;
  bool start_temporal_transition;
  bool temporal_transition_packet;
  sfu_pacer_class_t pacer_class;
} sfu_layer_scheduler_decision_t;

typedef struct sfu_layer_scheduler_slot {
  uint64_t publisher_id;
  sfu_layer_scheduler_t sched;
} sfu_layer_scheduler_slot_t;

void sfu_layer_scheduler_init(sfu_layer_scheduler_t *sched, uint32_t initial_publisher);
sfu_layer_scheduler_t *sfu_layer_scheduler_for_stream(sfu_peer_session_t *session, uint32_t publisher_id, sfu_media_kind_t source);
sfu_layer_scheduler_t *sfu_layer_scheduler_for(sfu_peer_session_t *session, uint32_t publisher_id);
sfu_pacer_class_t sfu_layer_scheduler_classify_frame(const sfu_layer_scheduler_t *sched, const sfu_svc_descriptor_t *desc);
bool sfu_layer_scheduler_prepare_packet(sfu_layer_scheduler_t *sched, const sfu_svc_descriptor_t *desc, bool is_keyframe,
                                        sfu_layer_scheduler_decision_t *decision);
void sfu_layer_scheduler_commit_packet(sfu_layer_scheduler_t *sched, const sfu_layer_scheduler_decision_t *decision);
void sfu_layer_scheduler_reject_packet(sfu_layer_scheduler_t *sched, const sfu_layer_scheduler_decision_t *decision);
void sfu_layer_scheduler_set_bitrate(sfu_layer_scheduler_t *sched, uint32_t bitrate_bps);
void sfu_layer_scheduler_switch_source(sfu_peer_session_t *session, uint32_t new_publisher_id);

#endif /* SFU_MEDIA_SVC_LAYER_SCHEDULER_H */
