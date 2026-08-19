#include "room/room_media_graph.h"
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include "peer/session.h"
#include "util/alloc.h"
#include "util/log.h"

#define SFU_DEFERRED_CAP (SFU_ROOM_MAX_PEERS * 6)

typedef enum {
  SFU_RECLAIM_RECEIVERS = 0,
  SFU_RECLAIM_AUDIO,
  SFU_RECLAIM_VIDEO,
} sfu_reclaim_kind_t;

typedef struct {
  void *ptr;
  sfu_reclaim_kind_t kind;
} sfu_deferred_entry_t;

typedef struct {
  sfu_deferred_entry_t entries[SFU_DEFERRED_CAP];
  uint32_t count;
} sfu_deferred_reclaim_t;

static void deferred_init(sfu_deferred_reclaim_t *d) { d->count = 0; }

static void deferred_push(sfu_deferred_reclaim_t *d, void *ptr, sfu_reclaim_kind_t kind) {
  if (!ptr || d->count >= SFU_DEFERRED_CAP) {
    return;
  }
  d->entries[d->count].ptr = ptr;
  d->entries[d->count].kind = kind;
  d->count++;
}

static void deferred_flush(sfu_deferred_reclaim_t *d) {
  for (uint32_t i = 0; i < d->count; i++) {
    switch (d->entries[i].kind) {
      case SFU_RECLAIM_RECEIVERS:
        sfu_snapshot_reclaim_receivers((sfu_receiver_snapshot_t *)d->entries[i].ptr);
        break;
      case SFU_RECLAIM_AUDIO:
        sfu_snapshot_reclaim_audio((sfu_audio_route_snapshot_t *)d->entries[i].ptr);
        break;
      case SFU_RECLAIM_VIDEO:
        sfu_snapshot_reclaim_video((sfu_video_route_snapshot_t *)d->entries[i].ptr);
        break;
    }
  }
  d->count = 0;
}

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

  pthread_mutex_lock(&media_source->media.lock);
  e->audio_ssrc = media_source->media.uplink_audio.ssrc;
  e->video_ssrc = media_source->media.uplink_video.ssrc;
  e->video_rtx_ssrc = media_source->media.uplink_video.rtx_ssrc;
  e->video_pt = media_source->media.uplink_video.payload_type;
  e->video_rtx_pt = media_source->media.uplink_video.rtx_payload_type;
  e->video_codec = media_source->media.uplink_video.codec;
  e->screen_ssrc = media_source->media.screen.ssrc;
  e->screen_rtx_ssrc = media_source->media.screen.rtx_ssrc;
  e->screen_pt = media_source->media.screen.payload_type;
  e->screen_rtx_pt = media_source->media.screen.rtx_payload_type;
  e->screen_codec = media_source->media.screen.codec;
  e->audio_active = media_source->media.uplink_audio.active;
  e->video_active = media_source->media.uplink_video.active;
  e->screen_active = media_source->media.screen.active;
  pthread_mutex_unlock(&media_source->media.lock);
}

static void snapshot_set_source_capabilities(sfu_receiver_entry_t *entry, sfu_peer_session_t *source) {
  bool source_is_audience = atomic_load_explicit(&source->is_audience, memory_order_acquire);
  entry->has_audio = true;
  entry->has_video = !source_is_audience;
  entry->has_screen = !source_is_audience;
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

static void snapshot_copy_route_mids(sfu_receiver_entry_t *entry, sfu_peer_session_t *subscriber, sfu_peer_session_t *publisher) {
  sfu_receiver_snapshot_t *subscriptions = sfu_session_subscriptions_acquire(subscriber);
  uint32_t pos = snapshot_find(subscriptions, publisher);
  if (pos != UINT32_MAX) {
    entry->mid_audio = subscriptions->entries[pos].mid_audio;
    entry->mid_video = subscriptions->entries[pos].mid_video;
    entry->mid_screen = subscriptions->entries[pos].mid_screen;
  }
  sfu_subscriptions_snapshot_release(subscriptions);
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
  snapshot_set_source_capabilities(&snap->entries[pos], fanout ? owner : dst);
  if (fanout) {
    snapshot_copy_route_mids(&snap->entries[pos], dst, owner);
  }

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
  e->screen_ssrc = 0;
  e->screen_rtx_ssrc = 0;
  e->audio_active = false;
  e->video_active = false;
  e->screen_active = false;

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
    e->mid_audio = atomic_fetch_add_explicit(&owner->graph.next_remote_mid, SFU_REMOTE_TRANSCEIVERS_PER_SLOT, memory_order_relaxed);
    e->mid_video = e->mid_audio + 1;
    e->mid_screen = e->mid_audio + 2;
  }
  snapshot_fill_entry(e, dst, fanout ? owner : dst);
  if (fanout) {
    snapshot_copy_route_mids(e, dst, owner);
  }
  snapshot_set_source_capabilities(e, fanout ? owner : dst);

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

static bool publish_split_fanout(sfu_peer_session_t *owner, const sfu_receiver_snapshot_t *combined, sfu_deferred_reclaim_t *d) {
  uint32_t audio_count = 0;
  uint32_t video_count = 0;
  uint32_t screen_count = 0;
  if (combined) {
    for (uint32_t i = 0; i < combined->count; i++) {
      audio_count += combined->entries[i].has_audio && combined->entries[i].audio_active;
      if (combined->entries[i].has_video && combined->entries[i].video_active && sfu_session_ensure_video_runtime(combined->entries[i].subscriber)) {
        video_count++;
      }
      if (combined->entries[i].has_screen && combined->entries[i].screen_active && sfu_session_ensure_video_runtime(combined->entries[i].subscriber)) {
        screen_count++;
      }
    }
  }

  SFU_LOG_INFO("fanout publish publisher_peer=%u combined=%u audio=%u video=%u screen=%u", owner->peer_id, combined ? combined->count : 0, audio_count,
               video_count, screen_count);

  sfu_audio_route_snapshot_t *audio = SFU_CALLOC(1, sizeof(*audio) + (size_t)audio_count * sizeof(audio->entries[0]));
  sfu_video_route_snapshot_t *video = SFU_CALLOC(1, sizeof(*video) + (size_t)video_count * sizeof(video->entries[0]));
  sfu_video_route_snapshot_t *screen = SFU_CALLOC(1, sizeof(*screen) + (size_t)screen_count * sizeof(screen->entries[0]));
  if (!audio || !video || !screen) {
    SFU_FREE(audio);
    SFU_FREE(video);
    SFU_FREE(screen);
    return false;
  }
  atomic_store_explicit(&audio->refcount, 1, memory_order_relaxed);
  atomic_store_explicit(&video->refcount, 1, memory_order_relaxed);
  atomic_store_explicit(&screen->refcount, 1, memory_order_relaxed);
  audio->generation = combined ? combined->generation : 0;
  video->generation = combined ? combined->generation : 0;
  screen->generation = combined ? combined->generation : 0;
  audio->count = audio->capacity = audio_count;
  video->count = video->capacity = video_count;
  screen->count = screen->capacity = screen_count;

  uint32_t audio_pos = 0;
  uint32_t video_pos = 0;
  uint32_t screen_pos = 0;
  if (combined) {
    for (uint32_t i = 0; i < combined->count; i++) {
      const sfu_receiver_entry_t *entry = &combined->entries[i];
      if (entry->has_audio && entry->audio_active) {
        sfu_audio_route_entry_t *route = &audio->entries[audio_pos++];
        route->subscriber = entry->subscriber;
        route->mid = entry->mid_audio;
        atomic_fetch_add_explicit(&entry->subscriber->refcount, 1, memory_order_relaxed);
        SFU_LOG_INFO("fanout audio route publisher_peer=%u subscriber_peer=%u subscriber_ufrag=%s active=%d mid=%u ssrc=%u", owner->peer_id,
                     entry->subscriber->peer_id, entry->subscriber->cold ? entry->subscriber->cold->ufrag : "", entry->audio_active, entry->mid_audio,
                     entry->audio_ssrc);
      }
      if (entry->has_video && entry->video_active && sfu_session_video_runtime_ready(entry->subscriber)) {
        sfu_video_route_entry_t *route = &video->entries[video_pos++];
        route->subscriber = entry->subscriber;
        route->video_ssrc = entry->video_ssrc;
        route->video_rtx_ssrc = entry->video_rtx_ssrc;
        route->mid = entry->mid_video;
        route->video_pt = entry->video_pt;
        route->video_rtx_pt = entry->video_rtx_pt;
        route->has_video = entry->has_video;
        atomic_fetch_add_explicit(&entry->subscriber->refcount, 1, memory_order_relaxed);
      }
      if (entry->has_screen && entry->screen_active && sfu_session_video_runtime_ready(entry->subscriber)) {
        sfu_video_route_entry_t *route = &screen->entries[screen_pos++];
        route->subscriber = entry->subscriber;
        route->video_ssrc = entry->screen_ssrc;
        route->video_rtx_ssrc = entry->screen_rtx_ssrc;
        route->mid = entry->mid_screen;
        route->video_pt = entry->screen_pt;
        route->video_rtx_pt = entry->screen_rtx_pt;
        route->has_video = entry->has_screen;
        atomic_fetch_add_explicit(&entry->subscriber->refcount, 1, memory_order_relaxed);
      }
    }
  }

  sfu_audio_route_snapshot_t *old_audio = sfu_session_publish_audio_fanout_swap(owner, audio);
  deferred_push(d, old_audio, SFU_RECLAIM_AUDIO);
  sfu_video_route_snapshot_t *old_video = sfu_session_publish_video_fanout_swap(owner, video);
  deferred_push(d, old_video, SFU_RECLAIM_VIDEO);
  sfu_video_route_snapshot_t *old_screen = sfu_session_publish_screen_fanout_swap(owner, screen);
  deferred_push(d, old_screen, SFU_RECLAIM_VIDEO);
  return true;
}

static void snapshot_replace(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap, sfu_deferred_reclaim_t *d) {
  sfu_receiver_snapshot_t *old = sfu_session_publish_receivers_swap(owner, new_snap);
  deferred_push(d, old, SFU_RECLAIM_RECEIVERS);
}

static void fanout_snapshot_replace(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *new_snap, sfu_deferred_reclaim_t *d) {
  if (!publish_split_fanout(owner, new_snap, d)) {
    SFU_LOG_ERROR("peer %u: failed to publish split fanout snapshots; using legacy graph", owner->peer_id);
    sfu_audio_route_snapshot_t *old_audio = sfu_session_publish_audio_fanout_swap(owner, NULL);
    deferred_push(d, old_audio, SFU_RECLAIM_AUDIO);
    sfu_video_route_snapshot_t *old_video = sfu_session_publish_video_fanout_swap(owner, NULL);
    deferred_push(d, old_video, SFU_RECLAIM_VIDEO);
    sfu_video_route_snapshot_t *old_screen = sfu_session_publish_screen_fanout_swap(owner, NULL);
    deferred_push(d, old_screen, SFU_RECLAIM_VIDEO);
  }
  sfu_receiver_snapshot_t *old = sfu_session_publish_fanout_targets_swap(owner, new_snap);
  deferred_push(d, old, SFU_RECLAIM_RECEIVERS);
}

static sfu_receiver_snapshot_t *snapshot_build_batch(sfu_peer_session_t *owner, sfu_peer_session_t **others, uint32_t count, bool fanout) {
  if (count == 0) {
    return NULL;
  }
  sfu_receiver_snapshot_t *snap = snapshot_alloc(count);
  if (!snap) {
    return NULL;
  }
  snap->generation = 1;
  snap->count = 0;

  for (uint32_t i = 0; i < count; i++) {
    sfu_peer_session_t *dst = others[i];
    sfu_receiver_entry_t *e = &snap->entries[snap->count];
    memset(e, 0, sizeof(*e));
    if (!fanout) {
      e->mid_audio = atomic_fetch_add_explicit(&owner->graph.next_remote_mid, SFU_REMOTE_TRANSCEIVERS_PER_SLOT, memory_order_relaxed);
      e->mid_video = e->mid_audio + 1;
      e->mid_screen = e->mid_audio + 2;
    }
    snapshot_fill_entry(e, dst, fanout ? owner : dst);
    if (fanout) {
      snapshot_copy_route_mids(e, dst, owner);
    }
    snapshot_set_source_capabilities(e, fanout ? owner : dst);
    snap->count++;
  }
  return snap;
}

void room_add_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);

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

  sfu_peer_session_t *recv_targets[SFU_ROOM_MAX_PEERS];
  uint32_t recv_count = 0;
  sfu_peer_session_t *fanout_targets[SFU_ROOM_MAX_PEERS];
  uint32_t fanout_count = 0;

  for (uint32_t i = 0; i + 1 < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == peer) {
      continue;
    }

    if (!sfu_session_accepts_work(other) || !sfu_session_accepts_work(peer)) {
      continue;
    }

    sfu_receiver_snapshot_t *snap = snapshot_build_with(other, peer, false);
    if (snap) {
      snapshot_replace(other, snap, &deferred);
    }
    recv_targets[recv_count++] = other;
    fanout_targets[fanout_count++] = other;
  }

  if (recv_count > 0) {
    sfu_receiver_snapshot_t *snap = snapshot_build_batch(peer, recv_targets, recv_count, false);
    if (snap) {
      snapshot_replace(peer, snap, &deferred);
    }
    for (uint32_t i = 0; i < recv_count; i++) {
      snap = snapshot_build_with(recv_targets[i], peer, true);
      if (snap) {
        fanout_snapshot_replace(recv_targets[i], snap, &deferred);
      }
    }
  }
  if (fanout_count > 0) {
    sfu_receiver_snapshot_t *snap = snapshot_build_batch(peer, fanout_targets, fanout_count, true);
    if (snap) {
      fanout_snapshot_replace(peer, snap, &deferred);
    }
  }

  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);
}

bool room_update_peer_role(sfu_room_t *room, sfu_peer_session_t *peer, bool is_audience) {
  if (!room || !peer) {
    return false;
  }

  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);

  pthread_mutex_lock(&room->lock);
  if (peer->room != room || atomic_load_explicit(&peer->is_audience, memory_order_acquire) == is_audience) {
    pthread_mutex_unlock(&room->lock);
    return false;
  }

  atomic_store_explicit(&peer->is_audience, is_audience, memory_order_release);
  atomic_store_explicit(&peer->media.ptt_active, false, memory_order_release);
  if (is_audience) {
    atomic_store_explicit(&peer->media.audio_send_negotiated, false, memory_order_release);
    atomic_store_explicit(&peer->media.video_send_negotiated, false, memory_order_release);
    atomic_store_explicit(&peer->media.screen_send_negotiated, false, memory_order_release);
    atomic_store_explicit(&peer->media.camera_enabled, false, memory_order_release);
    atomic_store_explicit(&peer->media.screen_enabled, false, memory_order_release);
    atomic_store_explicit(&peer->media.camera_rtp_observed, false, memory_order_release);
    atomic_store_explicit(&peer->media.screen_rtp_observed, false, memory_order_release);
  }

  pthread_mutex_lock(&peer->media.lock);
  if (is_audience) {
    peer->media.uplink_audio.ssrc = 0;
    peer->media.uplink_audio.active = false;
    peer->media.uplink_video.ssrc = 0;
    peer->media.uplink_video.rtx_ssrc = 0;
    peer->media.uplink_video.active = false;
    peer->media.screen.ssrc = 0;
    peer->media.screen.rtx_ssrc = 0;
    peer->media.screen.active = false;
  } else {
    if (peer->media.uplink_audio.ssrc != 0) {
      peer->media.uplink_audio.active = true;
    }
    (void)sfu_session_recompute_video_activity_locked(peer);
  }
  sfu_session_publish_media(peer);
  pthread_mutex_unlock(&peer->media.lock);

  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == peer || !sfu_session_accepts_work(other)) {
      continue;
    }

    if (is_audience) {
      sfu_receiver_snapshot_t *snap = snapshot_deactivate_entry(other, peer);
      if (snap) {
        snapshot_replace(other, snap, &deferred);
      }
      snap = snapshot_refresh_entry(peer, other, true);
      if (snap) {
        fanout_snapshot_replace(peer, snap, &deferred);
      }
    } else {
      sfu_receiver_snapshot_t *snap = snapshot_refresh_entry(other, peer, false);
      if (!snap) {
        snap = snapshot_build_with(other, peer, false);
      }
      if (snap) {
        snapshot_replace(other, snap, &deferred);
      }
      snap = snapshot_build_with(peer, other, true);
      if (snap) {
        fanout_snapshot_replace(peer, snap, &deferred);
      }
    }
  }

  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);
  return true;
}

void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);

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
      snapshot_replace(other, snap, &deferred);
    }
    snap = snapshot_build_without(other, peer, true);
    if (snap) {
      fanout_snapshot_replace(other, snap, &deferred);
    }
  }

  sfu_receiver_snapshot_t *empty = snapshot_alloc(0);
  if (empty) {
    empty->generation = 0;
    sfu_receiver_snapshot_t *old = sfu_session_publish_receivers_swap(peer, empty);
    deferred_push(&deferred, old, SFU_RECLAIM_RECEIVERS);
  } else {
    SFU_LOG_ERROR("room %" PRIu64 ": failed to allocate empty snapshot; clearing receivers in place", room->room_id);
    sfu_receiver_snapshot_t *old = sfu_session_publish_receivers_swap(peer, NULL);
    deferred_push(&deferred, old, SFU_RECLAIM_RECEIVERS);
  }
  empty = snapshot_alloc(0);
  if (empty) {
    empty->generation = 0;
  }
  fanout_snapshot_replace(peer, empty, &deferred);

  peer->room = NULL;

  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);
}

bool room_set_peer_ptt_active(sfu_room_t *room, sfu_peer_session_t *peer, bool active) {
  if (!room || !peer) {
    return false;
  }

  pthread_mutex_lock(&room->lock);
  bool allowed = peer->room == room && atomic_load_explicit(&peer->is_audience, memory_order_acquire) &&
                 (!active || atomic_load_explicit(&peer->media.audio_send_negotiated, memory_order_acquire));
  pthread_mutex_unlock(&room->lock);
  if (!allowed) {
    return false;
  }

  bool old_active = atomic_exchange_explicit(&peer->media.ptt_active, active, memory_order_acq_rel);
  pthread_mutex_lock(&peer->media.lock);
  bool audio_active = active && peer->media.uplink_audio.ssrc != 0;
  bool media_changed = peer->media.uplink_audio.active != audio_active;
  peer->media.uplink_audio.active = audio_active;
  if (media_changed) {
    sfu_session_publish_media(peer);
  }
  pthread_mutex_unlock(&peer->media.lock);

  if (old_active != active || media_changed) {
    room_refresh_peer_streams(room, peer);
  }
  return true;
}

void room_refresh_peer_streams(sfu_room_t *room, sfu_peer_session_t *updated_peer) {
  if (!room || !updated_peer) {
    return;
  }

  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);

  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->peer_count; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == updated_peer || !sfu_session_accepts_work(other)) {
      continue;
    }
    sfu_receiver_snapshot_t *snap = snapshot_refresh_entry(other, updated_peer, false);
    if (snap) {
      snapshot_replace(other, snap, &deferred);
    }
    snap = snapshot_refresh_entry(updated_peer, other, true);
    if (snap) {
      fanout_snapshot_replace(updated_peer, snap, &deferred);
    }
  }
  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);
}
