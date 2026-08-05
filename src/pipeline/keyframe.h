#ifndef SFU_PIPELINE_KEYFRAME_H
#define SFU_PIPELINE_KEYFRAME_H

#include "sfu/datadef.h"

typedef struct sfu_worker sfu_worker_t;

/* Request-side keyframe entry point used by the media pipeline. */
void sfu_worker_request_keyframe_throttled(sfu_worker_t *w, sfu_peer_session_t *publisher);

#endif /* SFU_PIPELINE_KEYFRAME_H */
