#include "room/room_media_graph.h"
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include "peer/session.h"
#include "util/alloc.h"
#include "util/log.h"

static void snapshot_fill_entry(sfu_receiver_entry_t *e, sfu_peer_session_t *dst) {
  atomic_fetch_add_explicit(&dst->refcount, 1, memory_order_relaxed);

  e->subscriber = dst;
  if (dst->cold) {
    snprintf(e->subscriber_ufrag, sizeof(e->subscriber_ufrag), "%s", dst->cold->ufrag);
  } else {
    e->subscriber_ufrag[0] = '\0';
  }

  e->audio_ssrc = dst->uplink_audio.ssrc;
  e->video_ssrc = dst->uplink_video.ssrc;
  e->video_rtx_ssrc = dst->uplink_video.rtx_ssrc;
  e->video_pt = dst->uplink_video.payload_type;
  e->video_rtx_pt = dst->uplink_video.rtx_payload_type;
  e->audio_active = dst->uplink_audio.active;
  e->video_active = dst->uplink_video.active;
}

/* Finds the slot position of destination `dst` in `old`, or UINT32_MAX. */
static uint32_t snapshot_find(const sfu_receiver_snapshot_t *old, const sfu_peer_session_t *dst) {
  if (!old) {
    return UINT32_MAX;
  }
  for (uint32_t i = 0; i < old->count; i++) {
    if (old->entries[i].subscriber == dst) {
      return i;
    }
  }
  return UINT32_MAX;
}

static sfu_receiver_snapshot_t *snapshot_alloc(uint32_t capacity) {
  sfu_receiver_snapshot_t *snap = SFU_CALLOC(1, sizeof(*snap) + (size_t)capacity * sizeof(snap->entries[0]));
  if (!snap) {
    return NULL;
  }
  atomic_store_explicit(&snap->refcount, 1, memory_order_relaxed);
  snap->capacity = capacity;
  return snap;
}

/* Builds "old snapshot + dst appended". MID numbers are preserved across
 * replacements for SDP stability; a brand-new destination consumes fresh MIDs
 * from the owner's counter. Returns NULL on allocation failure, leaving the
 * peer's published snapshot unchanged. Call with the room lock held.
 *
 * The room lock serializes writers, NOT readers: a concurrent reader can
 * still hold (or be releasing) the old snapshot, so the old snapshot must be
 * retained with sfu_session_receivers_acquire before its entries are read.
 * Without the retain there is a use-after-free window: the writer loads the
 * pointer, the last reader releases and frees it, the writer then reads
 * old->count/old->entries (found by the #86 churn stress test under TSan as
 * an allocator-recycle race). */
static sfu_receiver_snapshot_t *snapshot_build_with(sfu_peer_session_t *owner, sfu_peer_session_t *dst) {
  sfu_receiver_snapshot_t *old = sfu_session_receivers_acquire(owner);
  if (snapshot_find(old, dst) != UINT32_MAX) {
    sfu_receiver_snapshot_release(old);
    return NULL; /* already routed */
  }

  uint32_t old_count = old ? old->count : 0;
  sfu_receiver_snapshot_t *snap = snapshot_alloc(old_count + 1);
  if (!snap) {
    sfu_receiver_snapshot_release(old);
    return NULL;
  }
  snap->generation = (old ? old->generation : 0) + 1;

  for (uint32_t i = 0; i < old_count; i++) {
    snap->entries[i] = old->entries[i]; /* value copy; ref re-taken below */
    atomic_fetch_add_explicit(&old->entries[i].subscriber->refcount, 1, memory_order_relaxed);
  }

  sfu_receiver_entry_t *e = &snap->entries[old_count];
  memset(e, 0, sizeof(*e));
  /* snapshot_find returned UINT32_MAX above, so dst is a new destination and
   * consumes fresh MIDs from the owner's monotonic counter. */
  e->mid_audio = owner->next_remote_mid++;
  e->mid_video = owner->next_remote_mid++;
  snapshot_fill_entry(e, dst);
  e->has_audio = true;
  e->has_video = true;

  snap->count = old_count + 1;
  sfu_receiver_snapshot_release(old);
  return snap;
}

/* Builds "old snapshot minus dst". Slot positions and MID numbers of all
 * remaining destinations are preserved. Returns NULL on allocation failure,
 * leaving the peer's published snapshot unchanged. Call with room lock held.
 * Retains the old snapshot for the same reason as snapshot_build_with. */
static sfu_receiver_snapshot_t *snapshot_build_without(sfu_peer_session_t *owner, const sfu_peer_session_t *dst) {
  sfu_receiver_snapshot_t *old = sfu_session_receivers_acquire(owner);
  uint32_t pos = snapshot_find(old, dst);
  if (pos == UINT32_MAX) {
    sfu_receiver_snapshot_release(old);
    return NULL; /* dst not routed to owner */
  }

  uint32_t old_count = old->count;
  sfu_receiver_snapshot_t *snap = snapshot_alloc(old_count - 1);
  if (!snap) {
    sfu_receiver_snapshot_release(old);
    return NULL;
  }
  snap->generation = old->generation + 1;

  uint32_t out = 0;
  for (uint32_t i = 0; i < old_count; i++) {
    if (i == pos) {
      continue;
    }
    snap->entries[out] = old->entries[i]; /* value copy; ref re-taken below */
    atomic_fetch_add_explicit(&old->entries[i].subscriber->refcount, 1, memory_order_relaxed);
    out++;
  }
  snap->count = out;
  sfu_receiver_snapshot_release(old);
  return snap;
}

/* Replaces `owner`'s published snapshot with `new_snap` (taking ownership of
 * it) and marks the owner for renegotiation. Call with the room lock held. */
static void snapshot_replace(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) {
  sfu_session_publish_receivers(owner, new_snap);
  owner->negotiation_needed = true;
}

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer, sfu_scheduler_t *scheduler) {
  (void)scheduler; /* snapshots are self-refcounted; no epoch retirement */

  pthread_mutex_lock(&room->lock);

  if (peer->room == room) {
    pthread_mutex_unlock(&room->lock);
    return;
  }
  if (room->peer_count >= SFU_ROOM_MAX_PEERS) {
    pthread_mutex_unlock(&room->lock);
    SFU_LOG_WARN("room %" PRIu64 " full", room->room_id);
    return;
  }

  room->peers[room->peer_count++] = peer;
  peer->room = room;

  for (uint32_t i = 0; i + 1 < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == peer) {
      continue;
    }

    /* Routing is only wired between peers that both still accept work; a
     * closing peer is never added as a destination nor given new routes. */
    if (!sfu_session_accepts_work(other) || !sfu_session_accepts_work(peer)) {
      continue;
    }

    /* other subscribes to the new peer's uplink */
    sfu_receiver_snapshot_t *snap = snapshot_build_with(other, peer);
    if (snap) {
      snapshot_replace(other, snap);
    } else {
      SFU_LOG_WARN("room %" PRIu64 ": failed to build receiver snapshot for peer subscribing to %s", room->room_id, peer->cold ? peer->cold->ufrag : "?");
    }

    /* the new peer subscribes to other's uplink */
    snap = snapshot_build_with(peer, other);
    if (snap) {
      snapshot_replace(peer, snap);
    } else {
      SFU_LOG_WARN("room %" PRIu64 ": failed to build receiver snapshot for %s subscribing to peer", room->room_id, peer->cold ? peer->cold->ufrag : "?");
    }
  }

  pthread_mutex_unlock(&room->lock);
}

void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  pthread_mutex_lock(&room->lock);

  if (peer->room != room) {
    /* Already detached (or never a member): idempotent no-op. peer->room is
     * only ever written under the room lock, so this check is race-free. */
    pthread_mutex_unlock(&room->lock);
    return;
  }

  uint32_t idx = UINT32_MAX;
  for (uint32_t i = 0; i < room->peer_count; i++) {
    if (room->peers[i] == peer) {
      idx = i;
      break;
    }
  }

  if (idx == UINT32_MAX) {
    /* Defensive: back-pointer says member but the array disagrees. */
    peer->room = NULL;
    pthread_mutex_unlock(&room->lock);
    return;
  }

  room->peers[idx] = room->peers[room->peer_count - 1];
  room->peers[room->peer_count - 1] = NULL;
  room->peer_count--;

  /* Rebuild every remaining peer's snapshot without the departing peer. */
  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other) {
      continue;
    }
    sfu_receiver_snapshot_t *snap = snapshot_build_without(other, peer);
    if (snap) {
      snapshot_replace(other, snap);
    }
  }

  /* The departing peer's own snapshot is dropped entirely. */
  sfu_receiver_snapshot_t *empty = snapshot_alloc(0);
  if (empty) {
    empty->generation = 0;
    sfu_session_publish_receivers(peer, empty);
  } else {
    SFU_LOG_ERROR("room %" PRIu64 ": failed to allocate empty snapshot; clearing receivers in place", room->room_id);
    sfu_session_publish_receivers(peer, NULL);
  }

  peer->room = NULL;
  peer->negotiation_needed = false;

  pthread_mutex_unlock(&room->lock);
}
