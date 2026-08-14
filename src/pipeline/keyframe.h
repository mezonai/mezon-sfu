#ifndef SFU_PIPELINE_KEYFRAME_H
#define SFU_PIPELINE_KEYFRAME_H

#include "sfu/datadef.h"

typedef struct sfu_worker sfu_worker_t;

void sfu_worker_request_keyframe_throttled_for_source(sfu_worker_t *w, sfu_peer_session_t *publisher, sfu_media_kind_t source);
void sfu_worker_request_keyframe_throttled(sfu_worker_t *w, sfu_peer_session_t *publisher);

#endif /* SFU_PIPELINE_KEYFRAME_H */
