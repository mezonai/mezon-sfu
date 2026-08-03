#ifndef SFU_ROOM_MEDIA_GRAPH_H
#define SFU_ROOM_MEDIA_GRAPH_H

#include "runtime/scheduler.h"
#include "sfu/datadef.h"

/* Room membership drives copy-on-write replacement of every affected peer's
 * immutable receiver snapshot (F-03/F-04). Both functions are no-ops for
 * sessions that no longer accept work (closing/closed), so a close racing
 * membership changes never resurrects routing state.
 *
 * `scheduler` is accepted for API compatibility; retirement no longer needs
 * the epoch reclaimer because snapshots are self-refcounted. */
void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer, sfu_scheduler_t *scheduler);
void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer);

#endif  // SFU_ROOM_MEDIA_GRAPH_H
