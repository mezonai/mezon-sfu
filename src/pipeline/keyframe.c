#include "pipeline/keyframe.h"

#include "peer/session.h"

void sfu_worker_request_keyframe_throttled(sfu_worker_t *w, sfu_peer_session_t *publisher) { sfu_session_request_keyframe(w, publisher, false); }
