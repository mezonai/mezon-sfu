#include "pipeline/keyframe.h"

#include "peer/session.h"

void sfu_worker_request_keyframe_throttled_for_source(sfu_worker_t *w, sfu_peer_session_t *publisher, sfu_media_kind_t source) {
  sfu_session_request_keyframe_for_source(w, publisher, false, source);
}

void sfu_worker_request_keyframe_throttled(sfu_worker_t *w, sfu_peer_session_t *publisher) {
  sfu_worker_request_keyframe_throttled_for_source(w, publisher, SFU_MEDIA_VIDEO);
}
