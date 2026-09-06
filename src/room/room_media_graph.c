#include "room/room_media_graph.h"
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include "peer/session.h"
#include "protocol/signaling/signaling.h"
#include "runtime/timer.h"
#include "util/log.h"

#define SFU_DEFERRED_CAP (SFU_ROOM_MAX_PEERS * 3)

typedef enum { SFU_RECLAIM_RECEIVERS = 0, SFU_RECLAIM_FANOUT } sfu_reclaim_kind_t;

typedef struct {
  void *ptr;
  sfu_reclaim_kind_t kind;
} sfu_deferred_entry_t;

typedef struct {
  sfu_deferred_entry_t entries[SFU_DEFERRED_CAP];
  uint32_t count;
} sfu_deferred_reclaim_t;

typedef struct {
  sfu_peer_session_t *owner;
  uint32_t slot;
  uint64_t generation;
} sfu_slot_reservation_t;

static void deferred_init(sfu_deferred_reclaim_t *d) { d->count = 0; }

static void deferred_push(sfu_deferred_reclaim_t *d, void *ptr, sfu_reclaim_kind_t kind) {
  if (!ptr) {
    return;
  }
  if (d->count >= SFU_DEFERRED_CAP) {
    SFU_LOG_ERROR("deferred snapshot reclaim capacity exceeded (%u)", SFU_DEFERRED_CAP);
    abort();
  }
  d->entries[d->count++] = (sfu_deferred_entry_t){ptr, kind};
}

static void deferred_flush(sfu_deferred_reclaim_t *d) {
  for (uint32_t i = 0; i < d->count; i++) {
    if (d->entries[i].kind == SFU_RECLAIM_RECEIVERS) {
      sfu_snapshot_reclaim_receivers(d->entries[i].ptr);
    } else {
      sfu_snapshot_reclaim_fanout(d->entries[i].ptr);
    }
  }
  d->count = 0;
}

static void snapshot_fill_entry(sfu_receiver_entry_t *e, sfu_peer_session_t *source) {
  e->subscriber = source;
  if (source->cold) {
    snprintf(e->subscriber_ufrag, sizeof(e->subscriber_ufrag), "%s", source->cold->ufrag);
  }
  e->publisher_user_id = source->user_id;
  e->publisher_peer_id = source->peer_id;
  pthread_mutex_lock(&source->media.lock);
  e->audio_ssrc = source->media.uplink_audio.ssrc;
  e->video_ssrc = source->media.uplink_video.ssrc;
  e->video_rtx_ssrc = source->media.uplink_video.rtx_ssrc;
  e->video_pt = source->media.uplink_video.payload_type;
  e->video_rtx_pt = source->media.uplink_video.rtx_payload_type;
  e->video_codec = source->media.uplink_video.codec;
  e->screen_ssrc = source->media.screen.ssrc;
  e->screen_rtx_ssrc = source->media.screen.rtx_ssrc;
  e->screen_pt = source->media.screen.payload_type;
  e->screen_rtx_pt = source->media.screen.rtx_payload_type;
  e->screen_codec = source->media.screen.codec;
  e->audio_active = source->media.uplink_audio.active;
  e->video_active = source->media.uplink_video.active;
  e->screen_active = source->media.screen.active;
  pthread_mutex_unlock(&source->media.lock);
  bool audience = atomic_load_explicit(&source->is_audience, memory_order_acquire);
  e->has_audio = true;
  e->has_video = !audience;
  e->has_screen = !audience;
}

static bool receiver_entry_sdp_changed(const sfu_receiver_entry_t *old, const sfu_receiver_entry_t *new) {
  return old->audio_ssrc != new->audio_ssrc || old->video_ssrc != new->video_ssrc || old->video_rtx_ssrc != new->video_rtx_ssrc ||
         old->screen_ssrc != new->screen_ssrc || old->screen_rtx_ssrc != new->screen_rtx_ssrc || old->video_pt != new->video_pt ||
         old->video_rtx_pt != new->video_rtx_pt || old->screen_pt != new->screen_pt || old->screen_rtx_pt != new->screen_rtx_pt ||
         old->video_codec != new->video_codec || old->screen_codec != new->screen_codec || old->has_audio != new->has_audio ||
         old->has_video != new->has_video || old->has_screen != new->has_screen || old->audio_active != new->audio_active ||
         old->video_active != new->video_active || old->screen_active != new->screen_active;
}

static uint32_t snapshot_find(const sfu_receiver_snapshot_t *old, const sfu_peer_session_t *source) {
  uint32_t slot;
  return sfu_receiver_snapshot_find_peer(old, source, &slot) ? slot : UINT32_MAX;
}

static sfu_receiver_snapshot_t *snapshot_refresh_entry(sfu_peer_session_t *owner, sfu_peer_session_t *source) {
  sfu_receiver_snapshot_t *old = sfu_session_subscriptions_acquire(owner);
  uint32_t slot = snapshot_find(old, source);
  if (slot == UINT32_MAX) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  sfu_receiver_entry_t entry = *sfu_receiver_snapshot_at(old, slot);
  snapshot_fill_entry(&entry, source);
  sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_copy_set(old, slot, &entry);
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static sfu_receiver_snapshot_t *snapshot_deactivate_entry(sfu_peer_session_t *owner, sfu_peer_session_t *source) {
  sfu_receiver_snapshot_t *old = sfu_session_subscriptions_acquire(owner);
  uint32_t slot = snapshot_find(old, source);
  if (slot == UINT32_MAX) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  sfu_receiver_entry_t entry = *sfu_receiver_snapshot_at(old, slot);
  entry.audio_ssrc = entry.video_ssrc = entry.video_rtx_ssrc = entry.screen_ssrc = entry.screen_rtx_ssrc = 0;
  entry.audio_active = entry.video_active = entry.screen_active = false;
  sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_copy_set(old, slot, &entry);
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static sfu_receiver_snapshot_t *snapshot_build_at(sfu_peer_session_t *owner, sfu_peer_session_t *source, uint32_t remote_slot, uint64_t assignment_generation) {
  sfu_receiver_snapshot_t *old = sfu_session_subscriptions_acquire(owner);
  if (snapshot_find(old, source) != UINT32_MAX || sfu_receiver_snapshot_at(old, remote_slot)) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  sfu_receiver_entry_t entry = {0};
  entry.remote_slot = remote_slot;
  entry.assignment_generation = assignment_generation;
  snapshot_fill_entry(&entry, source);
  sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_copy_set(old, remote_slot, &entry);
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static sfu_receiver_snapshot_t *snapshot_build_without(sfu_peer_session_t *owner, const sfu_peer_session_t *source, uint32_t *remote_slot,
                                                       uint64_t *assignment_generation) {
  sfu_receiver_snapshot_t *old = sfu_session_subscriptions_acquire(owner);
  uint32_t slot = snapshot_find(old, source);
  if (slot == UINT32_MAX) {
    sfu_subscriptions_snapshot_release(old);
    return NULL;
  }
  const sfu_receiver_entry_t *entry = sfu_receiver_snapshot_at(old, slot);
  if (remote_slot) {
    *remote_slot = entry->remote_slot;
  }
  if (assignment_generation) {
    *assignment_generation = entry->assignment_generation;
  }
  sfu_receiver_snapshot_t *snap = sfu_receiver_snapshot_copy_set(old, slot, NULL);
  sfu_subscriptions_snapshot_release(old);
  return snap;
}

static void snapshot_replace(sfu_peer_session_t *owner, sfu_receiver_snapshot_t *snap, sfu_deferred_reclaim_t *d) {
  deferred_push(d, sfu_session_publish_receivers_swap(owner, snap), SFU_RECLAIM_RECEIVERS);
}

static bool fanout_route_fill_from(sfu_fanout_route_t *route, uint8_t *eligibility, sfu_peer_session_t *publisher, sfu_peer_session_t *subscriber,
                                   const sfu_receiver_snapshot_t *prepared_subs) {
  sfu_receiver_snapshot_t *acquired = prepared_subs ? NULL : sfu_session_subscriptions_acquire(subscriber);
  const sfu_receiver_snapshot_t *subs = prepared_subs ? prepared_subs : acquired;
  const sfu_receiver_entry_t *entry = sfu_receiver_snapshot_find_peer(subs, publisher, NULL);
  if (!entry) {
    sfu_subscriptions_snapshot_release(acquired);
    return false;
  }
  memset(route, 0, sizeof(*route));
  route->subscriber = subscriber;
  route->remote_slot = entry->remote_slot;
  route->assignment_generation = entry->assignment_generation;
  pthread_mutex_lock(&publisher->media.lock);
  route->video_ssrc = publisher->media.uplink_video.ssrc;
  route->video_rtx_ssrc = publisher->media.uplink_video.rtx_ssrc;
  route->screen_ssrc = publisher->media.screen.ssrc;
  route->screen_rtx_ssrc = publisher->media.screen.rtx_ssrc;
  route->video_pt = publisher->media.uplink_video.payload_type;
  route->video_rtx_pt = publisher->media.uplink_video.rtx_payload_type;
  route->screen_pt = publisher->media.screen.payload_type;
  route->screen_rtx_pt = publisher->media.screen.rtx_payload_type;
  bool audio = publisher->media.uplink_audio.active, video = publisher->media.uplink_video.active, screen = publisher->media.screen.active;
  pthread_mutex_unlock(&publisher->media.lock);
  bool audience = atomic_load_explicit(&publisher->is_audience, memory_order_acquire);
  *eligibility = audio ? SFU_FANOUT_AUDIO : 0;
  if (!audience && (video || screen) && sfu_session_ensure_video_runtime(subscriber)) {
    if (video) {
      *eligibility |= SFU_FANOUT_VIDEO;
    }
    if (screen) {
      *eligibility |= SFU_FANOUT_SCREEN;
    }
  }
#ifdef SFU_DIAG_LOG
  SFU_LOG_INFO("fanout: fill pub=%u sub=%u remote_slot=%u gen=%" PRIu64 " elig=0x%x audio=%d video=%d screen=%d audience=%d", publisher->peer_id,
               subscriber->peer_id, entry->remote_slot, entry->assignment_generation, (unsigned)*eligibility, audio ? 1 : 0, video ? 1 : 0, screen ? 1 : 0,
               audience ? 1 : 0);
#endif
  sfu_subscriptions_snapshot_release(acquired);
  return true;
}

static sfu_fanout_bundle_t *fanout_change_from(sfu_peer_session_t *publisher, sfu_peer_session_t *subscriber, bool remove,
                                               const sfu_receiver_snapshot_t *prepared_subs) {
  sfu_fanout_bundle_t *old = sfu_session_fanout_acquire(publisher);
  uint32_t slot;
  bool found = sfu_fanout_bundle_find_peer(old, subscriber, &slot) != NULL;
  if (remove) {
    if (!found) {
      sfu_fanout_bundle_release(old);
      return NULL;
    }
  } else if (!found) {
    for (slot = 0; slot < SFU_MAX_REMOTE_SLOTS && sfu_fanout_bundle_at(old, slot); slot++) {
    }
    if (slot == SFU_MAX_REMOTE_SLOTS) {
      sfu_fanout_bundle_release(old);
      return NULL;
    }
  }
  sfu_fanout_route_t route;
  uint8_t eligibility = 0;
  const sfu_fanout_route_t *value = NULL;
  if (!remove) {
    if (!fanout_route_fill_from(&route, &eligibility, publisher, subscriber, prepared_subs)) {
      sfu_fanout_bundle_release(old);
      return NULL;
    }
    value = &route;
  }
  sfu_fanout_bundle_t *bundle = sfu_fanout_bundle_copy_set(old, slot, value, eligibility);
  sfu_fanout_bundle_release(old);
  return bundle;
}

static sfu_fanout_bundle_t *fanout_change(sfu_peer_session_t *publisher, sfu_peer_session_t *subscriber, bool remove) {
  return fanout_change_from(publisher, subscriber, remove, NULL);
}

static void fanout_replace(sfu_peer_session_t *owner, sfu_fanout_bundle_t *bundle, sfu_deferred_reclaim_t *d) {
  deferred_push(d, sfu_session_publish_fanout_swap(owner, bundle), SFU_RECLAIM_FANOUT);
}

static void membership_capture_subject(sfu_membership_event_t *event, const sfu_peer_session_t *peer) {
  sfu_media_snapshot_t media = sfu_session_load_media(peer);
  event->subject_peer_id = peer->peer_id;
  event->subject_user_id = peer->user_id;
  event->subject_is_audience = atomic_load_explicit(&peer->is_audience, memory_order_acquire);
  event->subject_is_mute = atomic_load_explicit(&peer->media.is_mute, memory_order_acquire);
  event->subject_camera_requested = atomic_load_explicit(&peer->media.camera_enabled, memory_order_acquire);
  event->subject_camera_active = media.video_active;
  event->subject_screen_requested = atomic_load_explicit(&peer->media.screen_enabled, memory_order_acquire);
  event->subject_screen_active = media.screen_active;
  if (peer->cold) {
    snprintf(event->subject_ufrag, sizeof(event->subject_ufrag), "%s", peer->cold->ufrag);
  }
}

static void membership_capture_member(sfu_membership_member_t *member, const sfu_peer_session_t *peer, const sfu_receiver_entry_t *entry) {
  sfu_media_snapshot_t media = sfu_session_load_media(peer);
  *member = (sfu_membership_member_t){.peer_id = peer->peer_id,
                                      .user_id = peer->user_id,
                                      .mid_audio = entry ? sfu_remote_slot_first_mid(entry->remote_slot) : 0,
                                      .mid_video = entry ? sfu_remote_slot_first_mid(entry->remote_slot) + 1 : 0,
                                      .mid_screen = entry ? sfu_remote_slot_first_mid(entry->remote_slot) + 2 : 0,
                                      .remote_slot = entry ? entry->remote_slot : UINT32_MAX,
                                      .assignment_generation = entry ? entry->assignment_generation : 0,
                                      .is_audience = atomic_load_explicit(&peer->is_audience, memory_order_acquire),
                                      .is_mute = atomic_load_explicit(&peer->media.is_mute, memory_order_acquire),
                                      .camera_requested = atomic_load_explicit(&peer->media.camera_enabled, memory_order_acquire),
                                      .camera_active = media.video_active,
                                      .screen_requested = atomic_load_explicit(&peer->media.screen_enabled, memory_order_acquire),
                                      .screen_active = media.screen_active};
  if (peer->cold) {
    snprintf(member->ufrag, sizeof(member->ufrag), "%s", peer->cold->ufrag);
  }
}

sfu_room_admission_result_t room_add_peer_result(sfu_room_t *room, sfu_peer_session_t *peer) {
  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);
  sfu_peer_session_t *targets[SFU_ROOM_MAX_PEERS];
  sfu_receiver_snapshot_t *target_receivers[SFU_ROOM_MAX_PEERS] = {0};
  sfu_fanout_bundle_t *target_fanouts[SFU_ROOM_MAX_PEERS] = {0};
  sfu_receiver_snapshot_t *peer_receivers = NULL;
  sfu_fanout_bundle_t *peer_fanout = NULL;
  sfu_slot_reservation_t reservations[SFU_ROOM_MAX_PEERS * 2];
  uint32_t reservation_count = 0;
  uint32_t target_slots[SFU_ROOM_MAX_PEERS], peer_slots[SFU_ROOM_MAX_PEERS];
  uint64_t target_generations[SFU_ROOM_MAX_PEERS], peer_generations[SFU_ROOM_MAX_PEERS];
  uint32_t target_count = 0;
  bool admitted = false;
  bool prepared = false;
  sfu_room_admission_result_t result = SFU_ROOM_ADMISSION_ERROR;
  sfu_membership_event_t *event = NULL;
  sfu_membership_reservation_t event_reservation = {0};

  pthread_mutex_lock(&peer->membership_lock);
  pthread_mutex_lock(&room->lock);
  if (peer->room == room) {
    goto out;
  }

  if (peer->room || !sfu_session_accepts_work(peer)) {
    goto out;
  }

  if (room->peer_count >= room->peer_capacity || room->free_count == 0) {
    result = SFU_ROOM_ADMISSION_CAPACITY;
    goto out;
  }

  for (uint32_t i = 0; i < room->peer_capacity; i++) {
    sfu_peer_session_t *other = room->occupied[i] ? room->peers[i] : NULL;
    if (other && other != peer && sfu_session_accepts_work(other)) {
      targets[target_count++] = other;
    }
  }

  bool peer_is_audience = atomic_load_explicit(&peer->is_audience, memory_order_acquire);
  for (uint32_t i = 0; i < target_count; i++) {
    target_slots[i] = UINT32_MAX;
    target_generations[i] = 0;
    peer_slots[i] = UINT32_MAX;
    peer_generations[i] = 0;
    if (!peer_is_audience) {
      if (!sfu_session_remote_slot_reserve(targets[i], peer->user_id, peer->peer_id, &target_slots[i], &target_generations[i])) {
        result = SFU_ROOM_ADMISSION_CAPACITY;
        goto out;
      }
      reservations[reservation_count++] = (sfu_slot_reservation_t){targets[i], target_slots[i], target_generations[i]};
    }
    if (!sfu_session_remote_slot_reserve(peer, targets[i]->user_id, targets[i]->peer_id, &peer_slots[i], &peer_generations[i])) {
      result = SFU_ROOM_ADMISSION_CAPACITY;
      goto out;
    }
    reservations[reservation_count++] = (sfu_slot_reservation_t){peer, peer_slots[i], peer_generations[i]};
  }

  event = sfu_membership_event_alloc();
  if (!event) {
    goto out;
  }
  event->kind = SFU_MEMBERSHIP_JOIN;
  event->room_id = room->room_id;
  event->participant_count = target_count + 1;
  membership_capture_subject(event, peer);
  peer_receivers = sfu_receiver_snapshot_alloc();
  if (!peer_receivers) {
    goto out;
  }

  peer_fanout = sfu_fanout_bundle_alloc();
  if (!peer_fanout) {
    goto out;
  }

  for (uint32_t i = 0; i < target_count; i++) {
    if (!peer_is_audience) {
      target_receivers[i] = snapshot_build_at(targets[i], peer, target_slots[i], target_generations[i]);
      if (!target_receivers[i]) {
        goto out;
      }
    }
    sfu_receiver_entry_t peer_entry = {0};
    peer_entry.remote_slot = peer_slots[i];
    peer_entry.assignment_generation = peer_generations[i];
    snapshot_fill_entry(&peer_entry, targets[i]);
    if (!sfu_receiver_snapshot_set(peer_receivers, peer_slots[i], &peer_entry)) {
      goto out;
    }
    target_fanouts[i] = fanout_change_from(targets[i], peer, false, peer_receivers);
    if (!target_fanouts[i]) {
      goto out;
    }
    if (!peer_is_audience) {
      sfu_fanout_route_t route;
      uint8_t eligibility;
      if (!fanout_route_fill_from(&route, &eligibility, peer, targets[i], target_receivers[i]) || !sfu_fanout_bundle_set(peer_fanout, i, &route, eligibility)) {
        goto out;
      }
    }
  }

  if (!sfu_session_accepts_work(peer)) {
    goto out;
  }

  for (uint32_t i = 0; i < target_count; i++) {
    if (!sfu_session_accepts_work(targets[i])) {
      goto out;
    }
  }

  event->member_count = target_count + 1;
  membership_capture_member(&event->members[0], peer, NULL);
  event->recipient_count = target_count + 1;
  atomic_fetch_add_explicit(&peer->refcount, 1, memory_order_relaxed);
  event->recipients[0] =
      (sfu_membership_recipient_t){.session = peer, .fd = peer->fd, .remote_slot = UINT32_MAX, .send_snapshot = true, .renegotiate = target_count > 0};
  for (uint32_t i = 0; i < target_count; i++) {
    const sfu_receiver_entry_t *joiner_entry = sfu_receiver_snapshot_at(peer_receivers, peer_slots[i]);
    const sfu_receiver_entry_t *recipient_entry = target_receivers[i] ? sfu_receiver_snapshot_at(target_receivers[i], target_slots[i]) : NULL;
    membership_capture_member(&event->members[i + 1], targets[i], joiner_entry);
    atomic_fetch_add_explicit(&targets[i]->refcount, 1, memory_order_relaxed);
    event->recipients[i + 1] = (sfu_membership_recipient_t){.session = targets[i],
                                                            .fd = targets[i]->fd,
                                                            .mid_audio = recipient_entry ? sfu_remote_slot_first_mid(recipient_entry->remote_slot) : 0,
                                                            .mid_video = recipient_entry ? sfu_remote_slot_first_mid(recipient_entry->remote_slot) + 1 : 0,
                                                            .mid_screen = recipient_entry ? sfu_remote_slot_first_mid(recipient_entry->remote_slot) + 2 : 0,
                                                            .remote_slot = recipient_entry ? recipient_entry->remote_slot : UINT32_MAX,
                                                            .assignment_generation = recipient_entry ? recipient_entry->assignment_generation : 0,
                                                            .send_delta = true,
                                                            .renegotiate = !peer_is_audience};
  }

  if (!sfu_signaling_reserve_membership_event(&event_reservation)) {
    goto out;
  }

  prepared = true;
  uint32_t room_slot = room->free_slots[room->free_count - 1];
  for (uint32_t i = 0; i < target_count; i++) {
    if (target_receivers[i]) {
      snapshot_replace(targets[i], target_receivers[i], &deferred);
      target_receivers[i] = NULL;
    }
    fanout_replace(targets[i], target_fanouts[i], &deferred);
    target_fanouts[i] = NULL;
  }
  snapshot_replace(peer, peer_receivers, &deferred);
  peer_receivers = NULL;
  fanout_replace(peer, peer_fanout, &deferred);
  peer_fanout = NULL;
  room->free_count--;
  room->peers[room_slot] = peer;
  room->occupied[room_slot] = 1;
  room->peer_count++;
  peer->room = room;
  peer->room_slot = room_slot;

  if (++room->membership_revision == 0) {
    room->membership_revision = 1;
  }
  event->room_revision = room->membership_revision;
  sfu_signaling_commit_membership_event(&event_reservation, event);
  event = NULL;
  admitted = true;
  result = SFU_ROOM_ADMISSION_OK;
#ifndef NDEBUG
  sfu_session_graph_assert_invariants(peer);
  for (uint32_t i = 0; i < target_count; i++) {
    sfu_session_graph_assert_invariants(targets[i]);
  }
#endif
out:
  if (!admitted) {
    sfu_signaling_cancel_membership_event(&event_reservation);
    for (uint32_t i = reservation_count; i > 0; i--) {
      sfu_session_remote_slot_retire(reservations[i - 1].owner, reservations[i - 1].slot, reservations[i - 1].generation);
    }
  }
  pthread_mutex_unlock(&room->lock);
  if (!prepared) {
    for (uint32_t i = 0; i < target_count; i++) {
      sfu_subscriptions_snapshot_release(target_receivers[i]);
      sfu_fanout_bundle_release(target_fanouts[i]);
    }
    sfu_subscriptions_snapshot_release(peer_receivers);
    sfu_fanout_bundle_release(peer_fanout);
  }

  deferred_flush(&deferred);
  if (!admitted) {
    sfu_membership_event_release(event);
  }
  pthread_mutex_unlock(&peer->membership_lock);
  return result;
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

  sfu_slot_reservation_t lazy_reservations[SFU_ROOM_MAX_PEERS];
  uint32_t lazy_res_count = 0;

  if (!is_audience) {
    for (uint32_t i = 0; i < room->peer_capacity; i++) {
      sfu_peer_session_t *other = room->peers[i];
      if (!other || other == peer || !sfu_session_accepts_work(other)) {
        continue;
      }
      sfu_receiver_snapshot_t *before = sfu_session_subscriptions_acquire(other);
      uint32_t slot = snapshot_find(before, peer);
      sfu_subscriptions_snapshot_release(before);
      if (slot == UINT32_MAX) {
        uint32_t new_slot;
        uint64_t new_gen;
        if (!sfu_session_remote_slot_reserve(other, peer->user_id, peer->peer_id, &new_slot, &new_gen)) {
          for (uint32_t r = lazy_res_count; r > 0; r--) {
            sfu_session_remote_slot_retire(lazy_reservations[r - 1].owner, lazy_reservations[r - 1].slot, lazy_reservations[r - 1].generation);
          }
          pthread_mutex_unlock(&room->lock);
          return false;
        }
        lazy_reservations[lazy_res_count++] = (sfu_slot_reservation_t){other, new_slot, new_gen};
      }
    }
  }

  atomic_store_explicit(&peer->is_audience, is_audience, memory_order_release);
  atomic_store_explicit(&peer->media.ptt_active, false, memory_order_release);
  if (is_audience) {
    atomic_store_explicit(&peer->media.audio_send_negotiated, false, memory_order_release);
    atomic_store_explicit(&peer->media.video_send_negotiated, false, memory_order_release);
    atomic_store_explicit(&peer->media.screen_send_negotiated, false, memory_order_release);
    atomic_store_explicit(&peer->media.screen_keyframe_recovery_pending, false, memory_order_release);
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

  for (uint32_t i = 0; i < room->peer_capacity; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == peer || !sfu_session_accepts_work(other)) {
      continue;
    }

    if (is_audience) {
      sfu_receiver_snapshot_t *snap = snapshot_deactivate_entry(other, peer);
      if (snap) {
        snapshot_replace(other, snap, &deferred);
      }
      sfu_fanout_bundle_t *fanout = fanout_change_from(peer, other, false, snap);
      if (fanout) {
        fanout_replace(peer, fanout, &deferred);
      }
    } else {
      sfu_receiver_snapshot_t *snap = NULL;
      sfu_receiver_snapshot_t *before = sfu_session_subscriptions_acquire(other);
      uint32_t slot = snapshot_find(before, peer);
      sfu_subscriptions_snapshot_release(before);
      if (slot != UINT32_MAX) {
        snap = snapshot_refresh_entry(other, peer);
      } else {
        for (uint32_t r = 0; r < lazy_res_count; r++) {
          if (lazy_reservations[r].owner == other) {
            snap = snapshot_build_at(other, peer, lazy_reservations[r].slot, lazy_reservations[r].generation);
            break;
          }
        }
      }
      if (snap) {
        snapshot_replace(other, snap, &deferred);
      }
      sfu_fanout_bundle_t *fanout = fanout_change_from(peer, other, false, snap);
      if (fanout) {
        fanout_replace(peer, fanout, &deferred);
      }
    }
  }

  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);
  return true;
}

void room_remove_peer_membership_locked(sfu_room_t *room, sfu_peer_session_t *peer) {
  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);
  sfu_membership_event_t *event = NULL;
  sfu_membership_reservation_t reservation = {0};

  pthread_mutex_lock(&room->lock);

  if (peer->room != room) {
    pthread_mutex_unlock(&room->lock);
    return;
  }

  uint32_t idx = peer->room_slot;
  if (idx >= room->peer_capacity || !room->occupied[idx] || room->peers[idx] != peer) {
    SFU_LOG_ERROR("room %" PRIu64 ": peer %u has invalid room slot %u", room->room_id, peer->peer_id, idx);
    peer->room = NULL;
    peer->room_slot = UINT32_MAX;
    pthread_mutex_unlock(&room->lock);
    return;
  }

  bool signaling_reserved = sfu_signaling_reserve_membership_event(&reservation);

  room->peers[idx] = NULL;
  room->occupied[idx] = 0;
  room->free_slots[room->free_count++] = (uint16_t)idx;
  room->peer_count--;
  room->membership_revision++;
  if (room->membership_revision == 0) {
    room->membership_revision = 1;
  }

  while (atomic_exchange_explicit(&peer->leave_event_in_use, true, memory_order_acq_rel)) {
    sched_yield();
  }
  event = peer->leave_event;
  assert(event != NULL);
  memset(event, 0, sizeof(*event));
  event->preallocated_storage = true;
  event->storage_owner = peer;
  atomic_fetch_add_explicit(&peer->refcount, 1, memory_order_relaxed);
  event->kind = SFU_MEMBERSHIP_LEAVE;
  event->room_id = room->room_id;
  event->room_revision = room->membership_revision;
  event->participant_count = room->peer_count;
  membership_capture_subject(event, peer);

  for (uint32_t i = 0; i < room->peer_capacity; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other) {
      continue;
    }
    if (event && event->recipient_count < SFU_ROOM_MAX_PEERS && sfu_session_accepts_work(other) && other->fd >= 0) {
      uint32_t mid_audio = 0, mid_video = 0, mid_screen = 0, event_slot = UINT32_MAX;
      uint64_t event_generation = 0;
      sfu_receiver_snapshot_t *before = sfu_session_subscriptions_acquire(other);
      const sfu_receiver_entry_t *entry = sfu_receiver_snapshot_find_peer(before, peer, NULL);
      if (entry) {
        event_slot = entry->remote_slot;
        event_generation = entry->assignment_generation;
        mid_audio = sfu_remote_slot_first_mid(event_slot);
        mid_video = mid_audio + 1;
        mid_screen = mid_audio + 2;
      }
      sfu_subscriptions_snapshot_release(before);
      atomic_fetch_add_explicit(&other->refcount, 1, memory_order_relaxed);
      event->recipients[event->recipient_count++] = (sfu_membership_recipient_t){.session = other,
                                                                                 .fd = other->fd,
                                                                                 .mid_audio = mid_audio,
                                                                                 .mid_video = mid_video,
                                                                                 .mid_screen = mid_screen,
                                                                                 .remote_slot = event_slot,
                                                                                 .assignment_generation = event_generation,
                                                                                 .send_delta = true,
                                                                                 .renegotiate = mid_audio != 0};
    }
    uint32_t retired_slot = UINT32_MAX;
    uint64_t retired_generation = 0;
    sfu_receiver_snapshot_t *snap = snapshot_build_without(other, peer, &retired_slot, &retired_generation);
    if (snap) {
      snapshot_replace(other, snap, &deferred);
    }
    sfu_fanout_bundle_t *fanout = fanout_change(other, peer, true);
    if (fanout) {
      fanout_replace(other, fanout, &deferred);
    }
    if (retired_slot != UINT32_MAX && !sfu_session_remote_slot_retire(other, retired_slot, retired_generation)) {
      SFU_LOG_ERROR("room %" PRIu64 ": failed to retire peer %u slot %u on peer %u", room->room_id, peer->peer_id, retired_slot, other->peer_id);
    }
  }

  sfu_receiver_snapshot_t *leaving = sfu_session_subscriptions_acquire(peer);
  if (leaving) {
    sfu_receiver_snapshot_iter_t iter;
    sfu_receiver_snapshot_iter_init(&iter, leaving);
    const sfu_receiver_entry_t *entry;
    while ((entry = sfu_receiver_snapshot_iter_next(&iter, NULL)) != NULL) {
      if (!sfu_session_remote_slot_retire(peer, entry->remote_slot, entry->assignment_generation)) {
        SFU_LOG_ERROR("room %" PRIu64 ": failed to retire peer %u remote slot %u", room->room_id, peer->peer_id, entry->remote_slot);
      }
    }
    sfu_subscriptions_snapshot_release(leaving);
  }

  sfu_receiver_snapshot_t *empty = sfu_receiver_snapshot_alloc();
  if (empty) {
    empty->generation = 0;
    sfu_receiver_snapshot_t *old = sfu_session_publish_receivers_swap(peer, empty);
    deferred_push(&deferred, old, SFU_RECLAIM_RECEIVERS);
  } else {
    SFU_LOG_ERROR("room %" PRIu64 ": failed to allocate empty snapshot; clearing receivers in place", room->room_id);
    sfu_receiver_snapshot_t *old = sfu_session_publish_receivers_swap(peer, NULL);
    deferred_push(&deferred, old, SFU_RECLAIM_RECEIVERS);
  }
  sfu_fanout_bundle_t *empty_fanout = sfu_fanout_bundle_alloc();
  if (!empty_fanout) {
    SFU_LOG_ERROR("room %" PRIu64 ": failed to allocate empty fanout; clearing root", room->room_id);
  }
  fanout_replace(peer, empty_fanout, &deferred);

  peer->room = NULL;
  peer->room_slot = UINT32_MAX;
  if (signaling_reserved) {
    sfu_signaling_commit_membership_event(&reservation, event);
  } else {
    sfu_membership_event_release(event);
  }
  event = NULL;
#ifndef NDEBUG
  sfu_session_graph_assert_invariants(peer);
  for (uint32_t i = 0; i < room->peer_capacity; i++) {
    if (room->occupied[i] && room->peers[i]) {
      sfu_session_graph_assert_invariants(room->peers[i]);
    }
  }
#endif

  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);
}

void room_remove_peer(sfu_room_t *room, sfu_peer_session_t *peer) {
  if (!room || !peer) {
    return;
  }
  pthread_mutex_lock(&peer->membership_lock);
  room_remove_peer_membership_locked(room, peer);
  pthread_mutex_unlock(&peer->membership_lock);
}

static bool room_refresh_peer_streams_locked(sfu_room_t *room, sfu_peer_session_t *updated_peer, sfu_deferred_reclaim_t *deferred,
                                             sfu_peer_session_t **renegotiate, uint32_t *renegotiate_count) {
  uint32_t updated_slot = updated_peer->room_slot;
  if (updated_peer->room != room || !sfu_session_accepts_work(updated_peer) || updated_slot >= room->peer_capacity || !room->occupied[updated_slot] ||
      room->peers[updated_slot] != updated_peer) {
    return false;
  }

  sfu_slot_reservation_t lazy_reservations[SFU_ROOM_MAX_PEERS];
  uint32_t lazy_res_count = 0;
  bool is_aud = atomic_load_explicit(&updated_peer->is_audience, memory_order_acquire);
  bool ptt = atomic_load_explicit(&updated_peer->media.ptt_active, memory_order_acquire);
  bool should_publish = !is_aud || ptt;

  if (should_publish) {
    for (uint32_t i = 0; i < room->peer_capacity; i++) {
      sfu_peer_session_t *other = room->peers[i];
      if (!other || other == updated_peer || !sfu_session_accepts_work(other)) {
        continue;
      }
      sfu_receiver_snapshot_t *before = sfu_session_subscriptions_acquire(other);
      uint32_t slot = snapshot_find(before, updated_peer);
      sfu_subscriptions_snapshot_release(before);
      if (slot == UINT32_MAX) {
        uint32_t new_slot;
        uint64_t new_gen;
        if (!sfu_session_remote_slot_reserve(other, updated_peer->user_id, updated_peer->peer_id, &new_slot, &new_gen)) {
          for (uint32_t r = lazy_res_count; r > 0; r--) {
            sfu_session_remote_slot_retire(lazy_reservations[r - 1].owner, lazy_reservations[r - 1].slot, lazy_reservations[r - 1].generation);
          }
          return false;
        }
        lazy_reservations[lazy_res_count++] = (sfu_slot_reservation_t){other, new_slot, new_gen};
      }
    }
  }

  for (uint32_t i = 0; i < room->peer_capacity; i++) {
    sfu_peer_session_t *other = room->peers[i];
    if (!other || other == updated_peer || !sfu_session_accepts_work(other)) {
      continue;
    }
    sfu_receiver_snapshot_t *before = sfu_session_subscriptions_acquire(other);
    uint32_t slot = snapshot_find(before, updated_peer);
    const sfu_receiver_entry_t *old_entry = slot == UINT32_MAX ? NULL : sfu_receiver_snapshot_at(before, slot);
    sfu_receiver_entry_t old_copy = {0};
    if (old_entry) {
      old_copy = *old_entry;
    }

    sfu_receiver_snapshot_t *snap = NULL;
    if (slot != UINT32_MAX) {
      snap = snapshot_refresh_entry(other, updated_peer);
    } else if (should_publish) {
      for (uint32_t r = 0; r < lazy_res_count; r++) {
        if (lazy_reservations[r].owner == other) {
          snap = snapshot_build_at(other, updated_peer, lazy_reservations[r].slot, lazy_reservations[r].generation);
          break;
        }
      }
    }

    if (snap) {
      snapshot_replace(other, snap, deferred);
    }
    sfu_fanout_bundle_t *fanout = fanout_change_from(updated_peer, other, false, snap);
    if (fanout) {
      fanout_replace(updated_peer, fanout, deferred);
    }
    if (old_entry) {
      const sfu_receiver_entry_t *new_entry = snap ? sfu_receiver_snapshot_find_peer(snap, updated_peer, NULL) : NULL;
      if (new_entry && receiver_entry_sdp_changed(&old_copy, new_entry) && *renegotiate_count < SFU_ROOM_MAX_PEERS) {
        atomic_fetch_add_explicit(&other->refcount, 1, memory_order_relaxed);
        renegotiate[(*renegotiate_count)++] = other;
      }
    } else if (snap) {
      if (*renegotiate_count < SFU_ROOM_MAX_PEERS) {
        atomic_fetch_add_explicit(&other->refcount, 1, memory_order_relaxed);
        renegotiate[(*renegotiate_count)++] = other;
      }
    }
    sfu_subscriptions_snapshot_release(before);
  }
#ifndef NDEBUG
  sfu_session_graph_assert_invariants(updated_peer);
  for (uint32_t i = 0; i < room->peer_capacity; i++) {
    if (room->occupied[i] && room->peers[i]) {
      sfu_session_graph_assert_invariants(room->peers[i]);
    }
  }
#endif
  return true;
}

bool room_set_peer_ptt_active(sfu_room_t *room, sfu_peer_session_t *peer, bool active) {
  if (!room || !peer) {
    return false;
  }

  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);
  sfu_peer_session_t *renegotiate[SFU_ROOM_MAX_PEERS];
  uint32_t renegotiate_count = 0;

  pthread_mutex_lock(&room->lock);
  bool in_room = peer->room == room;
  bool is_audience = atomic_load_explicit(&peer->is_audience, memory_order_acquire);
  bool allowed = in_room && is_audience;
  if (!allowed) {
    pthread_mutex_unlock(&room->lock);
#ifdef SFU_DIAG_LOG
    SFU_LOG_WARN("ptt: rejected peer=%u user_id=%" PRId64 " ufrag=%s active=%d in_room=%d is_audience=%d", peer->peer_id, peer->user_id,
                 peer->cold ? peer->cold->ufrag : "", active, in_room, is_audience);
#endif
    return false;
  }

  bool old_active = atomic_load_explicit(&peer->media.ptt_active, memory_order_acquire);
  atomic_store_explicit(&peer->media.ptt_active, active, memory_order_release);
  pthread_mutex_lock(&peer->media.lock);
  bool old_audio_active = peer->media.uplink_audio.active;
  bool audio_active = active && peer->media.uplink_audio.ssrc != 0;
  peer->media.uplink_audio.active = audio_active;

#ifdef SFU_DIAG_LOG
  uint32_t uplink_ssrc = peer->media.uplink_audio.ssrc;
#endif

  if (old_audio_active != audio_active) {
    sfu_session_publish_media(peer);
  }
  pthread_mutex_unlock(&peer->media.lock);

  if (!room_refresh_peer_streams_locked(room, peer, &deferred, renegotiate, &renegotiate_count)) {
    atomic_store_explicit(&peer->media.ptt_active, old_active, memory_order_release);
    pthread_mutex_lock(&peer->media.lock);
    peer->media.uplink_audio.active = old_audio_active;
    if (old_audio_active != audio_active) {
      sfu_session_publish_media(peer);
    }
    pthread_mutex_unlock(&peer->media.lock);
    pthread_mutex_unlock(&room->lock);
    deferred_flush(&deferred);
    return false;
  }

  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);

#ifdef SFU_DIAG_LOG
  SFU_LOG_INFO("ptt: applied peer=%u user_id=%" PRId64 " ufrag=%s active=%d old_active=%d uplink_ssrc=%u audio_active=%d", peer->peer_id, peer->user_id,
               peer->cold ? peer->cold->ufrag : "", active, old_active, uplink_ssrc, audio_active);
#endif

#ifdef SFU_DIAG_LOG
  if (active && !old_active) {
    sfu_ptt_diag_t *d = &peer->media.ptt_diag;
    atomic_fetch_add_explicit(&d->generation, 1, memory_order_relaxed);
    atomic_store_explicit(&d->activation_ts_us, (int64_t)sfu_now_us(), memory_order_relaxed);
    d->baseline_datagrams = atomic_load_explicit(&d->datagrams, memory_order_relaxed);
    d->baseline_srtp_ok = atomic_load_explicit(&d->srtp_ok, memory_order_relaxed);
    d->baseline_srtp_fail = atomic_load_explicit(&d->srtp_fail, memory_order_relaxed);
    d->baseline_audio_packets = atomic_load_explicit(&d->audio_packets, memory_order_relaxed);
    d->baseline_audio_gate_drops = atomic_load_explicit(&d->audio_gate_drops, memory_order_relaxed);
    d->baseline_router_admissions = atomic_load_explicit(&d->router_admissions, memory_order_relaxed);
    d->baseline_empty_fanout = atomic_load_explicit(&d->empty_fanout, memory_order_relaxed);
    d->baseline_route_dispatches = atomic_load_explicit(&d->route_dispatches, memory_order_relaxed);
    d->baseline_router_pending_skips = atomic_load_explicit(&d->router_pending_skips, memory_order_relaxed);
  } else if (!active && old_active) {
    sfu_ptt_diag_t *d = &peer->media.ptt_diag;
    int64_t activation_ts = atomic_load_explicit(&d->activation_ts_us, memory_order_relaxed);
    int64_t now_us = (int64_t)sfu_now_us();
    int64_t elapsed_us = activation_ts > 0 ? now_us - activation_ts : -1;
    uint64_t delta_datagrams = atomic_load_explicit(&d->datagrams, memory_order_relaxed) - d->baseline_datagrams;
    uint64_t delta_srtp_ok = atomic_load_explicit(&d->srtp_ok, memory_order_relaxed) - d->baseline_srtp_ok;
    uint64_t delta_srtp_fail = atomic_load_explicit(&d->srtp_fail, memory_order_relaxed) - d->baseline_srtp_fail;
    uint64_t delta_audio = atomic_load_explicit(&d->audio_packets, memory_order_relaxed) - d->baseline_audio_packets;
    uint64_t delta_gate = atomic_load_explicit(&d->audio_gate_drops, memory_order_relaxed) - d->baseline_audio_gate_drops;
    uint64_t delta_admitted = atomic_load_explicit(&d->router_admissions, memory_order_relaxed) - d->baseline_router_admissions;
    uint64_t delta_empty = atomic_load_explicit(&d->empty_fanout, memory_order_relaxed) - d->baseline_empty_fanout;
    uint64_t delta_dispatched = atomic_load_explicit(&d->route_dispatches, memory_order_relaxed) - d->baseline_route_dispatches;
    uint64_t delta_pending = atomic_load_explicit(&d->router_pending_skips, memory_order_relaxed) - d->baseline_router_pending_skips;
    sfu_ptt_diag_class_t cls = sfu_ptt_diag_classify(d);
    uint32_t gen = atomic_load_explicit(&d->generation, memory_order_relaxed);
    bool audio_negotiated = atomic_load_explicit(&peer->media.audio_send_negotiated, memory_order_relaxed);
    SFU_LOG_WARN("ptt_diag: peer=%u user_id=%" PRId64 " ufrag=%s gen=%u elapsed_us=%" PRId64
                 " audio_negotiated=%d class=%s"
                 " datagrams=%" PRIu64 " srtp_ok=%" PRIu64 " srtp_fail=%" PRIu64 " audio=%" PRIu64 " gate_drops=%" PRIu64 " admitted=%" PRIu64
                 " empty_fanout=%" PRIu64 " dispatched=%" PRIu64 " pending_skips=%" PRIu64,
                 peer->peer_id, peer->user_id, peer->cold ? peer->cold->ufrag : "", gen, elapsed_us, audio_negotiated, sfu_ptt_diag_class_name(cls),
                 delta_datagrams, delta_srtp_ok, delta_srtp_fail, delta_audio, delta_gate, delta_admitted, delta_empty, delta_dispatched, delta_pending);
  }
#endif

  for (uint32_t i = 0; i < renegotiate_count; i++) {
    sfu_signaling_trigger_peer_renegotiation(renegotiate[i]);
    sfu_session_release(renegotiate[i]);
  }
  return true;
}

void room_refresh_peer_streams(sfu_room_t *room, sfu_peer_session_t *updated_peer) {
  if (!room || !updated_peer) {
    return;
  }

  sfu_deferred_reclaim_t deferred;
  deferred_init(&deferred);

  sfu_peer_session_t *renegotiate[SFU_ROOM_MAX_PEERS];
  uint32_t renegotiate_count = 0;

  pthread_mutex_lock(&room->lock);
  (void)room_refresh_peer_streams_locked(room, updated_peer, &deferred, renegotiate, &renegotiate_count);
  pthread_mutex_unlock(&room->lock);
  deferred_flush(&deferred);

  for (uint32_t i = 0; i < renegotiate_count; i++) {
    sfu_signaling_trigger_peer_renegotiation(renegotiate[i]);
    sfu_session_release(renegotiate[i]);
  }
}
