#include "room/room_media_graph.h"
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include "peer/session.h"
#include "util/alloc.h"
#include "util/log.h"

static void snapshot_fill_entry(sfu_receiver_entry_t *e, sfu_peer_session_t *target, sfu_peer_session_t *media_source) {
  atomic_fetch_add_explicit(&target->refcount, 1, memory_order_relaxed);

  e->subscriber = target;
  if (media_source->cold) {
    snprintf(e->subscriber_ufrag, sizeof(e->subscriber_ufrag), "%s", media_source->cold->ufrag);
  } else {
    e->subscriber_ufrag[0] = '\0';
  }
  e->publisher_user_id = media_source->user_id;
  e->publisher_peer_id = media_source->peer_id;

  pthread_mutex_lock(&media_source->media_lock);
  e->audio_ssrc = media_source->uplink_audio.ssrc;
  e->video_ssrc = media_source->uplink_video.ssrc;
  e->video_rtx_ssrc = media_source->uplink_video.rtx_ssrc;
  e->video_pt = media_source->uplink_video.payload_type;
  e->video_rtx_pt = media_source->uplink_video.rtx_payload_type;
  e->video_codec = media_source->uplink_video.codec;
  e->audio_active = media_source->uplink_audio.active;
  e->video_active = media_source->uplink_video.active;
  pthread_mutex_unlock(&media_source->media_lock);
}

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

static sfu_receiver_snapshot_t *snapshot_refresh_entry(sfu_peer_session_t *owner, sfu_peer_session_t *dst, bool fanout) {
  sfu_receiver_snapshot_t *old = fanout ? sfu_session_fanout_targets_acquire(owner) : sfu_session_subscriptions_acquire(owner);
  uint32_t pos = snapshot_find(old, dst);
  if (pos == UINT32_MAX) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }

  uint32_t old_count = old->count;
  sfu_receiver_snapshot_t *snap = snapshot_alloc(old_count);
  if (!snap) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  snap->generation = old->generation + 1;

  for (uint32_t i = 0; i < old_count; i++) {
    snap->entries[i] = old->entries[i];
    if (i == pos) {
      continue;
    }
    atomic_fetch_add_explicit(&old->entries[i].subscriber->refcount, 1, memory_order_relaxed);
  }

  snapshot_fill_entry(&snap->entries[pos], dst, fanout ? owner : dst);

  snap->count = old_count;
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static sfu_receiver_snapshot_t *snapshot_deactivate_entry(sfu_peer_session_t *owner, sfu_peer_session_t *dst) {
  sfu_receiver_snapshot_t *old = sfu_session_subscriptions_acquire(owner);
  uint32_t pos = snapshot_find(old, dst);
  if (pos == UINT32_MAX) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }

  uint32_t old_count = old->count;
  sfu_receiver_snapshot_t *snap = snapshot_alloc(old_count);
  if (!snap) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  snap->generation = old->generation + 1;

  for (uint32_t i = 0; i < old_count; i++) {
    snap->entries[i] = old->entries[i];
    atomic_fetch_add_explicit(&old->entries[i].subscriber->refcount, 1, memory_order_relaxed);
  }

  sfu_receiver_entry_t *e = &snap->entries[pos];
  e->audio_ssrc = 0;
  e->video_ssrc = 0;
  e->video_rtx_ssrc = 0;
  e->audio_active = false;
  e->video_active = false;

  snap->count = old_count;
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static sfu_receiver_snapshot_t *snapshot_build_with(sfu_peer_session_t *owner, sfu_peer_session_t *dst, bool fanout) {
  sfu_receiver_snapshot_t *old = fanout ? sfu_session_fanout_targets_acquire(owner) : sfu_session_subscriptions_acquire(owner);
  if (snapshot_find(old, dst) != UINT32_MAX) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }

  uint32_t old_count = old ? old->count : 0;
  sfu_receiver_snapshot_t *snap = snapshot_alloc(old_count + 1);
  if (!snap) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  snap->generation = (old ? old->generation : 0) + 1;

  for (uint32_t i = 0; i < old_count; i++) {
    snap->entries[i] = old->entries[i];
    atomic_fetch_add_explicit(&old->entries[i].subscriber->refcount, 1, memory_order_relaxed);
  }

  sfu_receiver_entry_t *e = &snap->entries[old_count];
  memset(e, 0, sizeof(*e));
  if (!fanout) {
    e->mid_audio = atomic_fetch_add_explicit(&owner->next_remote_mid, 2, memory_order_relaxed);
    e->mid_video = e->mid_audio + 1;
  }
  snapshot_fill_entry(e, dst, fanout ? owner : dst);
  e->has_audio = true;
  e->has_video = true;

  snap->count = old_count + 1;
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static sfu_receiver_snapshot_t *snapshot_build_without(sfu_peer_session_t *owner, const sfu_peer_session_t *dst, bool fanout) {
  sfu_receiver_snapshot_t *old = fanout ? sfu_session_fanout_targets_acquire(owner) : sfu_session_subscriptions_acquire(owner);
  uint32_t pos = snapshot_find(old, dst);
  if (pos == UINT32_MAX) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }

  uint32_t old_count = old->count;
  sfu_receiver_snapshot_t *snap = snapshot_alloc(old_count - 1);
  if (!snap) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  snap->generation = old->generation + 1;

  uint32_t out = 0;
  for (uint32_t i = 0; i < old_count; i++) {
    if (i == pos) {
      continue;
    }
    snap->entries[out] = old->entries[i];
    atomic_fetch_add_explicit(&old->entries[i].subscriber->refcount, 1, memory_order_relaxed);
    out++;
  }
  snap->count = out;
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static bool publish_split_fanout(sfu_peer_session_t *owner, const sfu_receiver_snapshot_t *combined) {
  uint32_t audio_count = 0;
  uint32_t video_count = 0;
  if (combined) {
    for (uint32_t i = 0; i < combined->count; i++) {
      audio_count += combined->entries[i].has_audio && combined->entries[i].audio_active;
      if (combined->entries[i].has_video && combined->entries[i].video_active && sfu_session_ensure_video_runtime(combined->entries[i].subscriber)) {
        video_count++;
      }
    }
  }

  sfu_audio_route_snapshot_t *audio = SFU_CALLOC(1, sizeof(*audio) + (size_t)audio_count * sizeof(audio->entries[0]));
  sfu_video_route_snapshot_t *video = SFU_CALLOC(1, sizeof(*video) + (size_t)video_count * sizeof(video->entries[0]));
  if (!audio || !video) {
    SFU_FREE(audio);
    SFU_FREE(video);
    return false;
  }
  atomic_store_explicit(&audio->refcount, 1, memory_order_relaxed);
  atomic_store_explicit(&video->refcount, 1, memory_order_relaxed);
  audio->generation = combined ? combined->generation : 0;
  video->generation = combined ? combined->generation : 0;
  audio->count = audio->capacity = audio_count;
  video->count = video->capacity = video_count;

  uint32_t audio_pos = 0;
  uint32_t video_pos = 0;
  if (combined) {
    for (uint32_t i = 0; i < combined->count; i++) {
      const sfu_receiver_entry_t *entry = &combined->entries[i];
      if (entry->has_audio && entry->audio_active) {
        audio->entries[audio_pos++].subscriber = entry->subscriber;
        atomic_fetch_add_explicit(&entry->subscriber->refcount, 1, memory_order_relaxed);
      }
      if (entry->has_video && entry->video_active && sfu_session_video_runtime_ready(entry->subscriber)) {
        sfu_video_route_entry_t *route = &video->entries[video_pos++];
        route->subscriber = entry->subscriber;
        route->video_ssrc = entry->video_ssrc;
        route->video_rtx_ssrc = entry->video_rtx_ssrc;
        route->video_pt = entry->video_pt;
        route->video_rtx_pt = entry->video_rtx_pt;
        route->has_video = entry->has_video;
        atomic_fetch_add_explicit(&entry->subscriber->refcount, 1, memory_order_relaxed);
      }
    }
  }

  sfu_session_publish_audio_fanout(owner, audio);
  sfu_session_publish_video_fanout(owner, video);
  return true;
}

static void snapshot_replace(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) { sfu_session_publish_receivers(owner, new_snap); }

static void fanout_snapshot_replace(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap) {
  if (!publish_split_fanout(owner, new_snap)) {
    SFU_LOG_ERROR("peer %u: failed to publish split fanout snapshots; using legacy graph", owner->peer_id);
    sfu_session_publish_audio_fanout(owner, NULL);
    sfu_session_publish_video_fanout(owner, NULL);
  }
  sfu_session_publish_fanout_targets(owner, new_snap);
}

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  pthread_mutex_lock(&room->lock);

  if (peer->room == room) {
    pthread_mutex_unlock(&room->lock);
    return;
  }
  if (peer->room) {
    SFU_LOG_WARN("peer %u already belongs to room %" PRIu64 "; refusing add to room %" PRIu64, peer->peer_id, ((sfu_room_t *)peer->room)->room_id,
                 room->room_id);
    pthread_mutex_unlock(&room->lock);
    return;
  }
  if (!sfu_session_accepts_work(peer)) {
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
  if (!sfu_session_accepts_work(peer)) {
    room->peers[--room->peer_count] = NULL;
    peer->room = NULL;
    pthread_mutex_unlock(&room->lock);
    return;
  }

  for (uint32_t i = 0; i + 1 < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == peer) {
      continue;
    }

    if (!sfu_session_accepts_work(other) || !sfu_session_accepts_work(peer)) {
      continue;
    }

    bool peer_is_audience = atomic_load_explicit(&peer->is_audience, memory_order_acquire);
    bool other_is_audience = atomic_load_explicit(&other->is_audience, memory_order_acquire);

    if (!peer_is_audience) {
      sfu_receiver_snapshot_t *snap = snapshot_build_with(other, peer, false);
      if (snap) {
        snapshot_replace(other, snap);
      }
    }
    if (!other_is_audience) {
      sfu_receiver_snapshot_t *snap = snapshot_build_with(peer, other, false);
      if (snap) {
        snapshot_replace(peer, snap);
      }
    }

    if (!peer_is_audience) {
      sfu_receiver_snapshot_t *snap = snapshot_build_with(peer, other, true);
      if (snap) {
        fanout_snapshot_replace(peer, snap);
      }
    }
    if (!other_is_audience) {
      sfu_receiver_snapshot_t *snap = snapshot_build_with(other, peer, true);
      if (snap) {
        fanout_snapshot_replace(other, snap);
      }
    }
  }

  pthread_mutex_unlock(&room->lock);
}

bool room_update_peer_role(sfu_room_t *room, sfu_peer_session_t *peer, bool is_audience) {
  if (!room || !peer) {
    return false;
  }

  pthread_mutex_lock(&room->lock);
  if (peer->room != room || atomic_load_explicit(&peer->is_audience, memory_order_acquire) == is_audience) {
    pthread_mutex_unlock(&room->lock);
    return false;
  }

  atomic_store_explicit(&peer->is_audience, is_audience, memory_order_release);
  if (is_audience) {
    atomic_store_explicit(&peer->audio_send_negotiated, false, memory_order_release);
    atomic_store_explicit(&peer->video_send_negotiated, false, memory_order_release);
  }

  pthread_mutex_lock(&peer->media_lock);
  if (is_audience) {
    peer->uplink_audio.active = false;
    peer->uplink_video.active = false;
  } else {
    if (peer->uplink_audio.ssrc != 0) {
      peer->uplink_audio.active = true;
    }
    if (peer->uplink_video.ssrc != 0) {
      peer->uplink_video.active = true;
    }
  }
  sfu_session_publish_media(peer);
  pthread_mutex_unlock(&peer->media_lock);

  if (is_audience) {
    sfu_receiver_snapshot_t *empty = snapshot_alloc(0);
    if (empty) {
      empty->generation = 0;
    }
    fanout_snapshot_replace(peer, empty);
  }

  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == peer || !sfu_session_accepts_work(other)) {
      continue;
    }

    if (is_audience) {
      sfu_receiver_snapshot_t *snap = snapshot_deactivate_entry(other, peer);
      if (snap) {
        snapshot_replace(other, snap);
      }
    } else {
      sfu_receiver_snapshot_t *snap = snapshot_refresh_entry(other, peer, false);
      if (!snap) {
        snap = snapshot_build_with(other, peer, false);
      }
      if (snap) {
        snapshot_replace(other, snap);
      }
      snap = snapshot_build_with(peer, other, true);
      if (snap) {
        fanout_snapshot_replace(peer, snap);
      }
    }
  }

  peer->negotiation_needed = true;
  pthread_mutex_unlock(&room->lock);
  return true;
}

void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  pthread_mutex_lock(&room->lock);

  if (peer->room != room) {
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
    peer->room = NULL;
    pthread_mutex_unlock(&room->lock);
    return;
  }

  room->peers[idx] = room->peers[room->peer_count - 1];
  room->peers[room->peer_count - 1] = NULL;
  room->peer_count--;

  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other) {
      continue;
    }
    sfu_receiver_snapshot_t *snap = snapshot_build_without(other, peer, false);
    if (snap) {
      snapshot_replace(other, snap);
    }
    snap = snapshot_build_without(other, peer, true);
    if (snap) {
      fanout_snapshot_replace(other, snap);
    }
  }

  sfu_receiver_snapshot_t *empty = snapshot_alloc(0);
  if (empty) {
    empty->generation = 0;
    sfu_session_publish_receivers(peer, empty);
  } else {
    SFU_LOG_ERROR("room %" PRIu64 ": failed to allocate empty snapshot; clearing receivers in place", room->room_id);
    sfu_session_publish_receivers(peer, NULL);
  }
  empty = snapshot_alloc(0);
  if (empty) {
    empty->generation = 0;
  }
  fanout_snapshot_replace(peer, empty);

  peer->room = NULL;
  peer->negotiation_needed = false;

  pthread_mutex_unlock(&room->lock);
}

void room_refresh_peer_streams(sfu_room_t *room, sfu_peer_session_t *updated_peer) {
  if (!room || !updated_peer) {
    return;
  }

  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == updated_peer || !sfu_session_accepts_work(other)) {
      continue;
    }
    sfu_receiver_snapshot_t *snap = snapshot_refresh_entry(other, updated_peer, false);
    if (snap) {
      snapshot_replace(other, snap);
    }
    snap = snapshot_refresh_entry(updated_peer, other, true);
    if (snap) {
      fanout_snapshot_replace(updated_peer, snap);
    }
  }
  pthread_mutex_unlock(&room->lock);
}
