#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "peer/session.h"
#include "protocol/signaling/signaling.h"
#include "room/room.h"
#include "room/room_media_graph.h"
#include "util/alloc.h"

static sfu_signaling_server_t signaling_test_server;

/* Heap-allocated refcounted mock session (no DTLS/RTX; construction bypasses
 * the session table because these tests exercise the room graph, not DTLS). */
static sfu_peer_session_t *mock_session(const char *ufrag) {
  sfu_peer_session_t *s = SFU_CALLOC(1, sizeof(*s));
  assert(s != NULL);
  s->cold = SFU_CALLOC(1, sizeof(*s->cold));
  assert(s->cold != NULL);
  s->room_slot = UINT32_MAX;
  snprintf(s->cold->ufrag, sizeof(s->cold->ufrag), "%s", ufrag);
  static _Atomic uint32_t next_peer_id = 1;
  s->peer_id = atomic_fetch_add_explicit(&next_peer_id, 1, memory_order_relaxed);
  s->user_id = s->peer_id;
  s->active = true;
  assert(pthread_mutex_init(&s->answer_lock, NULL) == 0);
  assert(pthread_mutex_init(&s->negotiation.lock, NULL) == 0);
  assert(pthread_mutex_init(&s->media.lock, NULL) == 0);
  assert(pthread_mutex_init(&s->graph.lock, NULL) == 0);
  assert(pthread_mutex_init(&s->crypto_lock, NULL) == 0);
  assert(pthread_mutex_init(&s->ingress_lock, NULL) == 0);
  assert(pthread_mutex_init(&s->membership_lock, NULL) == 0);
  s->leave_event = SFU_CALLOC(1, sizeof(*s->leave_event));
  assert(s->leave_event != NULL);
  s->leave_event->preallocated_storage = true;
  s->leave_event->storage_owner = s;
  atomic_store(&s->refcount, 1);
  atomic_store(&s->lifecycle, SFU_SESSION_LIFECYCLE_OPEN);
  atomic_store(&s->accepts_work, true);
  atomic_store(&s->media.visible, true);
  s->media.uplink_audio.owner = s;
  s->media.uplink_video.owner = s;
  s->media.uplink_audio.active = true;
  s->media.uplink_video.active = true;
  return s;
}

static uint32_t receiver_count(sfu_peer_session_t *peer) {
  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(peer);
  uint32_t n = snap ? snap->count : 0;
  sfu_subscriptions_snapshot_release(snap);
  return n;
}

static uint32_t fanout_target_count(sfu_peer_session_t *peer) {
  sfu_fanout_bundle_t *bundle = sfu_session_fanout_acquire(peer);
  uint32_t n = bundle ? bundle->count : 0;
  sfu_fanout_bundle_release(bundle);
  return n;
}

static bool fanout_has_peer(sfu_peer_session_t *peer, sfu_peer_session_t *dest) {
  sfu_fanout_bundle_t *bundle = sfu_session_fanout_acquire(peer);
  bool found = sfu_fanout_bundle_find_peer(bundle, dest, NULL) != NULL;
  sfu_fanout_bundle_release(bundle);
  return found;
}

static uint32_t route_count(sfu_peer_session_t *peer, sfu_media_kind_t kind) {
  sfu_fanout_bundle_t *bundle = sfu_session_fanout_acquire(peer);
  sfu_fanout_iter_t iter; sfu_fanout_iter_init(&iter, bundle, kind);
  uint32_t n = 0; while (sfu_fanout_iter_next(&iter, NULL)) n++;
  sfu_fanout_bundle_release(bundle); return n;
}
static uint32_t audio_route_count(sfu_peer_session_t *peer) { return route_count(peer, SFU_MEDIA_AUDIO); }
static uint32_t video_route_count(sfu_peer_session_t *peer) { return route_count(peer, SFU_MEDIA_VIDEO); }

static bool subscribes_to(sfu_peer_session_t *peer, sfu_peer_session_t *dest) {
  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(peer);
  bool found = false;
  if (snap) {
    sfu_receiver_snapshot_iter_t iter;
    sfu_receiver_snapshot_iter_init(&iter, snap);
    const sfu_receiver_entry_t *entry;
    while ((entry = sfu_receiver_snapshot_iter_next(&iter, NULL)) != NULL) {
      if (entry->subscriber == dest) {
        found = true;
        break;
      }
    }
  }
  sfu_subscriptions_snapshot_release(snap);
  return found;
}

static void test_add_remove(void) {
  sfu_room_t room;

  assert(sfu_room_init(&room, 1) == 0);

  sfu_peer_session_t *a = mock_session("a");
  sfu_peer_session_t *b = mock_session("b");
  sfu_peer_session_t *c = mock_session("c");

  room_add_peer(&room, a);
  assert(a->room_slot == 0);
  assert(room.occupied[a->room_slot]);
  assert(receiver_count(a) == 0);

  room_add_peer(&room, b);
  assert(receiver_count(a) == 1);
  assert(receiver_count(b) == 1);
  assert(subscribes_to(a, b));
  assert(subscribes_to(b, a));
  assert(audio_route_count(a) == 1 && audio_route_count(b) == 1);
  assert(video_route_count(a) == 1 && video_route_count(b) == 1);

  b->media.uplink_video.active = false;
  room_refresh_peer_streams(&room, b);
  assert(receiver_count(a) == 1);    /* stable SDP slot remains */
  assert(video_route_count(b) == 0); /* inactive source omitted from hot path */
  assert(audio_route_count(b) == 1);
  b->media.uplink_video.active = true;
  room_refresh_peer_streams(&room, b);
  assert(video_route_count(b) == 1);

  room_add_peer(&room, c);
  uint32_t a_slot = a->room_slot;
  uint32_t b_slot = b->room_slot;
  uint32_t c_slot = c->room_slot;
  assert(a_slot != b_slot && a_slot != c_slot && b_slot != c_slot);
  assert(receiver_count(a) == 2);
  assert(receiver_count(b) == 2);
  assert(receiver_count(c) == 2);
  assert(subscribes_to(a, b));
  assert(subscribes_to(a, c));

  /* MID numbers are stable for surviving entries across a removal. */
  uint32_t a_mid_for_c_audio = 0, a_mid_for_c_video = 0;
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(a);
    assert(snap != NULL);
    sfu_receiver_snapshot_iter_t iter;
    sfu_receiver_snapshot_iter_init(&iter, snap);
    const sfu_receiver_entry_t *entry;
    while ((entry = sfu_receiver_snapshot_iter_next(&iter, NULL)) != NULL) {
      if (entry->subscriber == c) {
        a_mid_for_c_audio = sfu_remote_slot_first_mid(entry->remote_slot);
        a_mid_for_c_video = sfu_remote_slot_first_mid(entry->remote_slot) + 1;
      }
    }
    sfu_subscriptions_snapshot_release(snap);
  }
  assert(a_mid_for_c_audio != 0 || a_mid_for_c_video != 0);

  room_remove_peer(&room, b);
  assert(b->room_slot == UINT32_MAX);
  assert(!room.occupied[b_slot]);
  assert(room.peers[b_slot] == NULL);
  assert(a->room_slot == a_slot && c->room_slot == c_slot);
  assert(room.peers[a_slot] == a && room.peers[c_slot] == c);
  assert(receiver_count(a) == 1);
  assert(receiver_count(c) == 1);
  assert(!subscribes_to(a, b));
  assert(subscribes_to(a, c));
  assert(b->room == NULL);
  assert(receiver_count(b) == 0);

  /* c's MIDs inside a's snapshot survived the rebuild. */
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(a);
    assert(snap != NULL && snap->count == 1);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).subscriber == c);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) == a_mid_for_c_audio);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) + 1 == a_mid_for_c_video);
    sfu_subscriptions_snapshot_release(snap);
  }

  sfu_room_t other_room;
  assert(sfu_room_init(&other_room, 2) == 0);
  room_add_peer(&other_room, a);
  assert(a->room == &room);
  assert(other_room.peer_count == 0);
  sfu_room_destroy(&other_room);

  room_remove_peer(&room, b);
  assert(receiver_count(a) == 1);

  /* Closing peer c (accepts_work=false) keeps it out of future snapshots. */
  atomic_store(&c->accepts_work, false);
  room_add_peer(&room, b);
  assert(b->room_slot == b_slot);
  assert(a->room_slot == a_slot && c->room_slot == c_slot);
  /* re-add occupies the freed sparse slot without moving surviving peers */
  assert(receiver_count(b) == 1); /* b sees a only */
  assert(!subscribes_to(b, c));
  assert(subscribes_to(a, b)); /* a still accepts work, so it routes to b */
  assert(receiver_count(a) == 2);

  sfu_session_release(a);
  sfu_session_release(b);
  sfu_session_release(c);

  sfu_room_destroy(&room);
}

/* Old snapshot stays valid for a holder across a concurrent replacement. */
static void test_audience_role_asymmetry_and_transition(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 4) == 0);

  sfu_peer_session_t *speaker = mock_session("speaker");
  speaker->media.uplink_audio.ssrc = 1111;
  speaker->media.uplink_video.ssrc = 2222;
  speaker->media.uplink_video.rtx_ssrc = 3333;
  speaker->media.uplink_video.payload_type = 96;
  speaker->media.uplink_video.rtx_payload_type = 97;
  speaker->media.uplink_video.codec = SFU_VIDEO_CODEC_VP8;
  sfu_peer_session_t *audience = mock_session("audience");
  audience->media.screen.ssrc = 4444;
  audience->media.screen.rtx_ssrc = 5555;
  audience->media.screen.payload_type = 96;
  audience->media.screen.rtx_payload_type = 97;
  audience->media.screen.codec = SFU_VIDEO_CODEC_VP8;
  audience->media.screen.active = false;
  audience->media.uplink_audio.ssrc = 7777;
  audience->media.uplink_audio.active = false;
  audience->media.uplink_video.active = false;
  atomic_store(&audience->media.audio_send_negotiated, true);
  atomic_store(&audience->is_audience, true);

  room_add_peer(&room, speaker);
  room_add_peer(&room, audience);

  /* Audience receives the speaker SDP source but never owns an RTP fanout
   * snapshot. The speaker forwards to the audience. */
  assert(receiver_count(audience) == 1);
  assert(sfu_session_remote_slot_high_water(audience) == 1);
  assert(subscribes_to(audience, speaker));
  assert(receiver_count(speaker) == 1);
  assert(subscribes_to(speaker, audience));
  assert(fanout_target_count(speaker) == 1);
  assert(fanout_has_peer(speaker, audience));
  assert(fanout_target_count(audience) == 1);
  assert(fanout_has_peer(audience, speaker));
  uint32_t audience_mid_before_ptt = 0;
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    audience_mid_before_ptt = sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).has_audio);
    assert(!(*sfu_receiver_snapshot_nth(snap, 0, NULL)).has_video);
    assert(!(*sfu_receiver_snapshot_nth(snap, 0, NULL)).has_screen);
    assert(!(*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_active);
    sfu_subscriptions_snapshot_release(snap);
  }
  uint32_t speaker_slots_before_ptt = sfu_session_remote_slot_high_water(speaker);
  assert(audio_route_count(audience) == 0);
  assert(room_set_peer_ptt_active(&room, audience, true));
  assert(atomic_load(&audience->is_audience));
  assert(atomic_load(&audience->media.ptt_active));
  assert(audio_route_count(audience) == 1);
  assert(sfu_session_remote_slot_high_water(speaker) == speaker_slots_before_ptt);
  assert(room_set_peer_ptt_active(&room, audience, false));
  assert(!atomic_load(&audience->media.ptt_active));
  assert(audio_route_count(audience) == 0);
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) == audience_mid_before_ptt);
    assert(!(*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_active);
    sfu_subscriptions_snapshot_release(snap);
  }
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(audience);
    assert(snap != NULL && snap->count == 1);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).subscriber == speaker);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) == SFU_REMOTE_MID_BASE);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) + 1 == SFU_REMOTE_MID_BASE + 1);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) + 2 == SFU_REMOTE_MID_BASE + 2);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_active);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_active);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_ssrc == 1111);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_ssrc == 2222);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_rtx_ssrc == 3333);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_pt == 96);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_rtx_pt == 97);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_codec == SFU_VIDEO_CODEC_VP8);
    sfu_subscriptions_snapshot_release(snap);
  }

  assert(room_update_peer_role(&room, audience, false));
  assert(!atomic_load(&audience->is_audience));
  assert(receiver_count(speaker) == 1);
  assert(subscribes_to(speaker, audience));
  assert(fanout_target_count(audience) == 1);
  assert(fanout_has_peer(audience, speaker));

  assert(room_update_peer_role(&room, audience, true));
  assert(atomic_load(&audience->is_audience));
  /* Demotion keeps the subscription slot (deactivated) so its mids stay
   * stable for a later re-promotion; the audience no longer owns fanout
   * targets but the speaker still receives from it. */
  assert(receiver_count(speaker) == 1);
  assert(fanout_target_count(audience) == 1);
  assert(fanout_has_peer(speaker, audience));

  /* The deactivated slot has its media state cleared... */
  uint32_t mid_audio = 0, mid_video = 0, mid_screen = 0;
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).subscriber == audience);
    assert(!(*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_active);
    assert(!(*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_active);
    assert(!(*sfu_receiver_snapshot_nth(snap, 0, NULL)).screen_active);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_ssrc == 0);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_ssrc == 0);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).screen_ssrc == 0);
    mid_audio = sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot);
    mid_video = sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) + 1;
    mid_screen = sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) + 2;
    sfu_subscriptions_snapshot_release(snap);
  }
  /* ...and the demoted peer's uplink state is reset so a re-promotion with
   * fresh browser SSRCs is learned from RTP again. */
  assert(audience->media.uplink_audio.ssrc == 0);
  assert(!audience->media.uplink_audio.active);
  assert(audience->media.uplink_video.ssrc == 0);
  assert(!audience->media.uplink_video.active);
  assert(audience->media.screen.ssrc == 0);
  assert(!audience->media.screen.active);

  /* Re-promotion with fresh uplink SSRCs reuses the same slot: mids are
   * preserved (the subscriber's transceivers stay bound) while the new
   * media state is picked up. */
  audience->media.uplink_audio.ssrc = 1111;
  audience->media.uplink_audio.active = true;
  audience->media.uplink_video.ssrc = 2222;
  audience->media.uplink_video.rtx_ssrc = 3333;
  audience->media.uplink_video.active = true;
  audience->media.screen.ssrc = 4444;
  audience->media.screen.rtx_ssrc = 5555;
  audience->media.screen.payload_type = 96;
  audience->media.screen.rtx_payload_type = 97;
  audience->media.screen.codec = SFU_VIDEO_CODEC_VP8;
  atomic_store(&audience->media.camera_enabled, true);
  atomic_store(&audience->media.screen_enabled, true);
  atomic_store(&audience->media.camera_rtp_observed, true);
  atomic_store(&audience->media.screen_rtp_observed, true);
  atomic_store(&audience->media.video_send_negotiated, true);
  atomic_store(&audience->media.screen_send_negotiated, true);

  uint32_t speaker_slot_high_water = sfu_session_remote_slot_high_water(speaker);
  assert(room_update_peer_role(&room, audience, false));
  assert(receiver_count(speaker) == 1);
  assert(fanout_has_peer(audience, speaker));
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).subscriber == audience);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) == mid_audio);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) + 1 == mid_video);
    assert(sfu_remote_slot_first_mid((*sfu_receiver_snapshot_nth(snap, 0, NULL)).remote_slot) + 2 == mid_screen);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_active);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_active);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).screen_active);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).audio_ssrc == 1111);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_ssrc == 2222);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).video_rtx_ssrc == 3333);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).screen_ssrc == 4444);
    assert((*sfu_receiver_snapshot_nth(snap, 0, NULL)).screen_rtx_ssrc == 5555);
    sfu_subscriptions_snapshot_release(snap);
  }
  /* No fresh mid pair was allocated for the re-promoted peer. */
  assert(sfu_session_remote_slot_high_water(speaker) == speaker_slot_high_water);

  room_remove_peer(&room, audience);
  room_remove_peer(&room, speaker);
  sfu_session_release(audience);
  sfu_session_release(speaker);
  sfu_room_destroy(&room);
}

static void test_snapshot_hold_across_replace(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 2) == 0);

  sfu_peer_session_t *a = mock_session("a");
  sfu_peer_session_t *b = mock_session("b");
  sfu_peer_session_t *c = mock_session("c");

  room_add_peer(&room, a);
  room_add_peer(&room, b);

  /* Hold the current snapshot of a. */
  sfu_receiver_snapshot_t *held = sfu_session_subscriptions_acquire(a);
  assert(held != NULL && held->count == 1);
  assert(sfu_session_remote_slot_high_water(a) == 1);
  assert((*sfu_receiver_snapshot_nth(held, 0, NULL)).subscriber == b);

  /* Writer publishes a replacement while we hold the old one. */
  room_add_peer(&room, c);

  /* The held (old) snapshot is unchanged and its entries are still valid. */
  assert(held->count == 1);
  assert((*sfu_receiver_snapshot_nth(held, 0, NULL)).subscriber == b);
  assert((*sfu_receiver_snapshot_nth(held, 0, NULL)).subscriber->cold != NULL);

  /* New acquisitions see the replacement. */
  sfu_receiver_snapshot_t *fresh = sfu_session_subscriptions_acquire(a);
  assert(fresh != NULL && fresh->count == 2);
  assert(fresh != held);
  assert(fresh->generation == held->generation + 1);
  assert(sfu_session_remote_slot_high_water(a) == 2);
  sfu_subscriptions_snapshot_release(fresh);

  room_remove_peer(&room, c);
  fresh = sfu_session_subscriptions_acquire(a);
  assert(fresh != NULL && fresh->count == 1);
  assert(sfu_session_remote_slot_high_water(a) == 2);
  sfu_subscriptions_snapshot_release(fresh);

  /* Releasing the held snapshot must not free b (still referenced by the new
   * snapshot and by our own pin). */
  sfu_subscriptions_snapshot_release(held);
  assert(b->cold != NULL);
  assert(subscribes_to(a, b));

  sfu_session_release(a);
  sfu_session_release(b);
  sfu_session_release(c);
  sfu_room_destroy(&room);
}

typedef struct {
  sfu_room_t *room;
  sfu_peer_session_t **peers;
  uint32_t peer_count;
  pthread_barrier_t barrier;
} snap_race_ctx_t;

static void *snap_reader(void *arg) {
  snap_race_ctx_t *ctx = arg;
  pthread_barrier_wait(&ctx->barrier);
  for (int i = 0; i < 500; i++) {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(ctx->peers[0]);
    if (snap) {
      sfu_receiver_snapshot_iter_t iter;
      sfu_receiver_snapshot_iter_init(&iter, snap);
      const sfu_receiver_entry_t *entry;
      while ((entry = sfu_receiver_snapshot_iter_next(&iter, NULL)) != NULL) {
        /* Entries must be coherent: retained subscriber with valid cold. */
        assert(entry->subscriber != NULL);
        assert(entry->subscriber->cold != NULL);
      }
      sfu_subscriptions_snapshot_release(snap);
    }
  }
  return NULL;
}

static void *snap_writer(void *arg) {
  snap_race_ctx_t *ctx = arg;
  pthread_barrier_wait(&ctx->barrier);
  for (int i = 0; i < 50; i++) {
    room_add_peer(ctx->room, ctx->peers[1]);
    room_remove_peer(ctx->room, ctx->peers[1]);
  }
  return NULL;
}

/* Concurrent snapshot reads vs copy-on-write replacement (TSan target). */
static void test_concurrent_snapshot_read_write(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 3) == 0);

  sfu_peer_session_t *peers[2];
  peers[0] = mock_session("reader-owner");
  peers[1] = mock_session("flapper");

  snap_race_ctx_t ctx = {
      .room = &room,
      .peers = peers,
      .peer_count = 2,
  };
  pthread_barrier_init(&ctx.barrier, NULL, 2);

  room_add_peer(&room, peers[0]);

  pthread_t reader, writer;
  assert(pthread_create(&reader, NULL, snap_reader, &ctx) == 0);
  assert(pthread_create(&writer, NULL, snap_writer, &ctx) == 0);
  pthread_join(reader, NULL);
  pthread_join(writer, NULL);

  pthread_barrier_destroy(&ctx.barrier);

  /* Final state: flapper removed, owner has empty snapshot. */
  assert(receiver_count(peers[0]) == 0);

  sfu_session_release(peers[0]);
  sfu_session_release(peers[1]);
  sfu_room_destroy(&room);
}

typedef struct {
  sfu_room_t *room;
  sfu_peer_session_t *publisher;
  sfu_peer_session_t *flapper;
  pthread_barrier_t barrier;
} fanout_race_ctx_t;

static void *fanout_churn_writer(void *arg) {
  fanout_race_ctx_t *ctx = arg;
  pthread_barrier_wait(&ctx->barrier);
  for (int i = 0; i < 100; i++) {
    room_add_peer(ctx->room, ctx->flapper);
    room_remove_peer(ctx->room, ctx->flapper);
  }
  return NULL;
}

static void *fanout_churn_reader(void *arg) {
  fanout_race_ctx_t *ctx = arg;
  pthread_barrier_wait(&ctx->barrier);
  for (int i = 0; i < 2000; i++) {
    sfu_fanout_bundle_t *bundle = sfu_session_fanout_acquire(ctx->publisher);
    sfu_fanout_iter_t iter;
    sfu_fanout_iter_init(&iter, bundle, SFU_MEDIA_AUDIO);
    uint32_t slot;
    const sfu_fanout_route_t *route;
    while ((route = sfu_fanout_iter_next(&iter, &slot)) != NULL) {
      assert(slot < SFU_MAX_REMOTE_SLOTS);
      assert(route == sfu_fanout_bundle_at(bundle, slot));
      assert(route->subscriber == ctx->flapper);
      assert(route->subscriber->cold != NULL);
    }
    sfu_fanout_bundle_release(bundle);
  }
  return NULL;
}

/* Acquired fanout bundles and their routes remain valid while room churn
 * concurrently replaces and reclaims the publisher's current root. */
static void test_concurrent_fanout_bundle_acquire_iterate_churn(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 12) == 0);
  sfu_peer_session_t *publisher = mock_session("fanout-publisher");
  sfu_peer_session_t *flapper = mock_session("fanout-flapper");
  room_add_peer(&room, publisher);

  fanout_race_ctx_t ctx = {.room = &room, .publisher = publisher, .flapper = flapper};
  assert(pthread_barrier_init(&ctx.barrier, NULL, 2) == 0);
  pthread_t reader, writer;
  assert(pthread_create(&reader, NULL, fanout_churn_reader, &ctx) == 0);
  assert(pthread_create(&writer, NULL, fanout_churn_writer, &ctx) == 0);
  pthread_join(reader, NULL);
  pthread_join(writer, NULL);
  pthread_barrier_destroy(&ctx.barrier);

  assert(fanout_target_count(publisher) == 0);
  room_remove_peer(&room, publisher);
  sfu_session_release(flapper);
  sfu_session_release(publisher);
  sfu_room_destroy(&room);
}

/* #86: multi-publisher room churn — one stable subscriber, several
 * publisher threads concurrently add/remove themselves while a reader
 * thread continuously acquires each publisher's receiver snapshot and
 * dereferences every entry (the forwarding hot path's access pattern).
 * Under TSan this witnesses the copy-on-write snapshot contract: entries
 * are always coherent (retained, non-NULL) no matter how replacement
 * interleaves with reads. */
typedef struct {
  sfu_room_t *room;
  sfu_peer_session_t **publishers;
  uint32_t publisher_count;
  sfu_peer_session_t *subscriber;
  pthread_barrier_t barrier;
} churn_ctx_t;

static void *churn_writer(void *arg) {
  churn_ctx_t *ctx = arg;
  /* Each writer owns exactly one publisher slot, assigned atomically. */
  static _Atomic intptr_t next_slot;
  intptr_t idx = atomic_fetch_add_explicit(&next_slot, 1, memory_order_relaxed);

  pthread_barrier_wait(&ctx->barrier);
  for (int i = 0; i < 100; i++) {
    room_add_peer(ctx->room, ctx->publishers[idx]);
    room_remove_peer(ctx->room, ctx->publishers[idx]);
  }
  return NULL;
}

static void *churn_reader(void *arg) {
  churn_ctx_t *ctx = arg;
  pthread_barrier_wait(&ctx->barrier);
  for (int i = 0; i < 2000; i++) {
    sfu_peer_session_t *pub = ctx->publishers[i % ctx->publisher_count];
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(pub);
    if (snap) {
      sfu_receiver_snapshot_iter_t iter;
      sfu_receiver_snapshot_iter_init(&iter, snap);
      const sfu_receiver_entry_t *entry;
      while ((entry = sfu_receiver_snapshot_iter_next(&iter, NULL)) != NULL) {
        /* Coherence: retained subscriber with valid cold, even mid-churn. */
        assert(entry->subscriber != NULL);
        assert(entry->subscriber->cold != NULL);
      }
      sfu_subscriptions_snapshot_release(snap);
    }
  }
  return NULL;
}

static void test_multi_publisher_concurrent_churn(void) {
  enum { PUBS = 4, WRITERS = PUBS, READERS = 2 };
  sfu_room_t room;
  assert(sfu_room_init(&room, 9) == 0);

  sfu_peer_session_t *pubs[PUBS];
  for (int i = 0; i < PUBS; i++) {
    char name[16];
    snprintf(name, sizeof(name), "pub%d", i);
    pubs[i] = mock_session(name);
  }
  sfu_peer_session_t *sub = mock_session("sub");

  room_add_peer(&room, sub);

  churn_ctx_t ctx = {
      .room = &room,
      .publishers = pubs,
      .publisher_count = PUBS,
      .subscriber = sub,
  };
  pthread_barrier_init(&ctx.barrier, NULL, WRITERS + READERS);

  pthread_t writers[WRITERS], readers[READERS];
  for (int i = 0; i < WRITERS; i++) {
    assert(pthread_create(&writers[i], NULL, churn_writer, &ctx) == 0);
  }
  for (int i = 0; i < READERS; i++) {
    assert(pthread_create(&readers[i], NULL, churn_reader, &ctx) == 0);
  }
  for (int i = 0; i < WRITERS; i++) {
    pthread_join(writers[i], NULL);
  }
  for (int i = 0; i < READERS; i++) {
    pthread_join(readers[i], NULL);
  }
  pthread_barrier_destroy(&ctx.barrier);

  /* All publishers flapped out; the subscriber's view of the room is
   * itself only, and each publisher has no receivers left. */
  for (int i = 0; i < PUBS; i++) {
    assert(receiver_count(pubs[i]) == 0);
    sfu_session_release(pubs[i]);
  }
  sfu_session_release(sub);
  sfu_room_destroy(&room);
}

static void test_fanout_uses_publisher_stream_identity(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 10) == 0);

  sfu_peer_session_t *publisher = mock_session("publisher");
  sfu_peer_session_t *subscriber = mock_session("subscriber");
  publisher->media.uplink_video.ssrc = 1111;
  publisher->media.uplink_video.rtx_ssrc = 2222;
  publisher->media.uplink_video.payload_type = 98;
  publisher->media.uplink_video.rtx_payload_type = 97;
  publisher->media.uplink_video.codec = SFU_VIDEO_CODEC_VP9;
  subscriber->media.uplink_video.ssrc = 3333;
  subscriber->media.uplink_video.rtx_ssrc = 4444;
  subscriber->media.uplink_video.payload_type = 96;
  subscriber->media.uplink_video.rtx_payload_type = 99;
  subscriber->media.uplink_video.codec = SFU_VIDEO_CODEC_VP8;

  room_add_peer(&room, publisher);
  room_add_peer(&room, subscriber);

  sfu_fanout_bundle_t *fanout = sfu_session_fanout_acquire(publisher);
  assert(fanout != NULL && fanout->count == 1);
  const sfu_fanout_route_t *route = sfu_fanout_bundle_find_peer(fanout, subscriber, NULL);
  assert(route != NULL && route->subscriber == subscriber);
  assert(route->video_ssrc == publisher->media.uplink_video.ssrc);
  assert(route->video_rtx_ssrc == publisher->media.uplink_video.rtx_ssrc);
  assert(sfu_remote_slot_first_mid(route->remote_slot) == SFU_REMOTE_MID_BASE);
  assert(sfu_remote_slot_first_mid(route->remote_slot) + 1 == SFU_REMOTE_MID_BASE + 1);
  assert(sfu_remote_slot_first_mid(route->remote_slot) + 2 == SFU_REMOTE_MID_BASE + 2);
  assert(route->video_pt == publisher->media.uplink_video.payload_type);
  assert(route->video_rtx_pt == publisher->media.uplink_video.rtx_payload_type);
  sfu_fanout_bundle_release(fanout);

  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(subscriber);
  assert(snap != NULL && snap->count == 1);
  const sfu_receiver_entry_t *entry = sfu_receiver_snapshot_nth(snap, 0, NULL);
  assert(entry->subscriber == publisher);
  assert(entry->video_ssrc == publisher->media.uplink_video.ssrc);
  assert(entry->video_codec == SFU_VIDEO_CODEC_VP9);
  sfu_subscriptions_snapshot_release(snap);

  room_remove_peer(&room, subscriber);
  room_remove_peer(&room, publisher);
  sfu_session_release(subscriber);
  sfu_session_release(publisher);
  sfu_room_destroy(&room);
}

static void assert_fanout_mids_match_subscriptions(sfu_peer_session_t *publisher, uint32_t expected_targets) {
  sfu_fanout_bundle_t *fanout = sfu_session_fanout_acquire(publisher);
  assert(fanout != NULL && fanout->count == expected_targets);
  for (uint32_t slot = 0; slot < SFU_MAX_REMOTE_SLOTS; slot++) {
    const sfu_fanout_route_t *route = sfu_fanout_bundle_at(fanout, slot);
    if (!route) continue;
    sfu_receiver_snapshot_t *subscriptions = sfu_session_subscriptions_acquire(route->subscriber);
    sfu_receiver_snapshot_iter_t subscription_iter;
    sfu_receiver_snapshot_iter_init(&subscription_iter, subscriptions);
    const sfu_receiver_entry_t *subscription = NULL;
    const sfu_receiver_entry_t *candidate;
    while ((candidate = sfu_receiver_snapshot_iter_next(&subscription_iter, NULL)) != NULL) {
      if (candidate->subscriber == publisher) {
        subscription = candidate;
        break;
      }
    }
    assert(subscription != NULL);
    assert(route->remote_slot == subscription->remote_slot);
    assert(route->assignment_generation == subscription->assignment_generation);
    assert(route->assignment_generation != 0);
    sfu_subscriptions_snapshot_release(subscriptions);
  }
  sfu_fanout_bundle_release(fanout);
}

static void test_three_peer_route_mids(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 11) == 0);
  sfu_peer_session_t *a = mock_session("a3");
  sfu_peer_session_t *b = mock_session("b3");
  sfu_peer_session_t *c = mock_session("c3");
  a->peer_id = 1;
  b->peer_id = 2;
  c->peer_id = 3;

  room_add_peer(&room, a);
  room_add_peer(&room, b);
  room_add_peer(&room, c);

  assert_fanout_mids_match_subscriptions(a, 2);
  assert_fanout_mids_match_subscriptions(b, 2);
  assert_fanout_mids_match_subscriptions(c, 2);
  assert(sfu_session_remote_slot_high_water(a) == 2);
  assert(sfu_session_remote_slot_high_water(b) == 2);
  assert(sfu_session_remote_slot_high_water(c) == 2);

  room_remove_peer(&room, c);
  room_remove_peer(&room, b);
  room_remove_peer(&room, a);
  sfu_session_release(c);
  sfu_session_release(b);
  sfu_session_release(a);
  sfu_room_destroy(&room);
}

static void test_add_rejects_exhausted_mid_capacity_transactionally(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 13) == 0);
  sfu_peer_session_t *member = mock_session("mid-full-member");
  sfu_peer_session_t *candidate = mock_session("mid-candidate");
  room_add_peer(&room, member);

  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    uint32_t slot;
    uint64_t generation;
    assert(sfu_session_remote_slot_reserve(member, 100000 + i, 100000 + i, &slot, &generation));
    assert(slot == i);
  }
  uint32_t candidate_high_water = sfu_session_remote_slot_high_water(candidate);
  uint32_t member_slot = member->room_slot;
  uint32_t free_count = room.free_count;

  assert(room_add_peer_result(&room, candidate) == SFU_ROOM_ADMISSION_CAPACITY);

  assert(room.peer_count == 1);
  assert(room.free_count == free_count);
  assert(room.peers[member_slot] == member && room.occupied[member_slot]);
  assert(candidate->room == NULL && candidate->room_slot == UINT32_MAX);
  assert(receiver_count(member) == 0 && fanout_target_count(member) == 0);
  assert(receiver_count(candidate) == 0 && fanout_target_count(candidate) == 0);
  assert(sfu_session_remote_slot_high_water(member) == SFU_MAX_REMOTE_SLOTS);
  assert(sfu_session_remote_slot_high_water(candidate) == candidate_high_water);

  room_remove_peer(&room, member);
  sfu_session_release(candidate);
  sfu_session_release(member);
  sfu_room_destroy(&room);
}

static void test_removed_peer_refresh_is_noop(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 14) == 0);
  sfu_peer_session_t *publisher = mock_session("removed-refresh-publisher");
  sfu_peer_session_t *subscriber = mock_session("removed-refresh-subscriber");
  room_add_peer(&room, publisher);
  room_add_peer(&room, subscriber);
  room_remove_peer(&room, publisher);
  assert(fanout_target_count(publisher) == 0);

  /* Model a stale subscription observed by a delayed refresh. A departed
   * publisher must not rebuild fanout even if such stale graph data exists. */
  sfu_receiver_snapshot_t *stale = sfu_receiver_snapshot_alloc();
  assert(stale != NULL);
  sfu_receiver_entry_t entry = {
      .subscriber = publisher,
      .remote_slot = 0,
      .assignment_generation = 1,
      .has_audio = true,
      .has_video = true,
  };
  assert(sfu_receiver_snapshot_set(stale, 0, &entry));
  sfu_snapshot_reclaim_receivers(sfu_session_publish_receivers_swap(subscriber, stale));

  room_refresh_peer_streams(&room, publisher);
  assert(fanout_target_count(publisher) == 0);

  sfu_snapshot_reclaim_receivers(sfu_session_publish_receivers_swap(subscriber, NULL));
  room_remove_peer(&room, subscriber);
  sfu_session_release(subscriber);
  sfu_session_release(publisher);
  sfu_room_destroy(&room);
}

static void test_chunked_subscription_root_copy(void) {
  sfu_peer_session_t *a = mock_session("chunk-a");
  sfu_peer_session_t *b = mock_session("chunk-b");
  sfu_receiver_snapshot_t *root = sfu_receiver_snapshot_alloc();
  assert(root != NULL);

  sfu_receiver_entry_t a_entry = {.subscriber = a, .remote_slot = 1, .assignment_generation = 1};
  sfu_receiver_entry_t b_entry = {.subscriber = b, .remote_slot = 40, .assignment_generation = 2};
  assert(sfu_receiver_snapshot_set(root, 1, &a_entry));
  assert(sfu_receiver_snapshot_set(root, 31, &a_entry));
  assert(sfu_receiver_snapshot_set(root, 40, &b_entry));
  assert(atomic_load(&a->refcount) == 3);
  assert(atomic_load(&b->refcount) == 2);
  assert(root->count == 3);
  assert(sfu_receiver_snapshot_at(root, 40)->subscriber == b);
  assert(sfu_receiver_snapshot_find_peer(root, a, NULL) == sfu_receiver_snapshot_at(root, 1));

  sfu_receiver_snapshot_iter_t iter;
  sfu_receiver_snapshot_iter_init(&iter, root);
  uint32_t remote_slot = UINT32_MAX;
  assert(sfu_receiver_snapshot_iter_next(&iter, &remote_slot) == sfu_receiver_snapshot_at(root, 1));
  assert(remote_slot == 1);
  assert(sfu_receiver_snapshot_iter_next(&iter, &remote_slot) == sfu_receiver_snapshot_at(root, 31));
  assert(remote_slot == 31);
  assert(sfu_receiver_snapshot_iter_next(&iter, &remote_slot) == sfu_receiver_snapshot_at(root, 40));
  assert(remote_slot == 40);
  assert(sfu_receiver_snapshot_iter_next(&iter, &remote_slot) == NULL);
  assert(sfu_receiver_snapshot_nth(root, 2, &remote_slot) == sfu_receiver_snapshot_at(root, 40));
  assert(remote_slot == 40);

  b_entry.audio_active = true;
  sfu_receiver_snapshot_t *copy = sfu_receiver_snapshot_copy_set(root, 40, &b_entry);
  assert(copy != NULL && copy->count == 3);
  assert(atomic_load(&a->refcount) == 3);
  assert(atomic_load(&b->refcount) == 3);
  assert(copy->chunks[0] == root->chunks[0]);
  assert(copy->chunks[1] != root->chunks[1]);
  assert(!sfu_receiver_snapshot_at(root, 40)->audio_active);
  assert(sfu_receiver_snapshot_at(copy, 40)->audio_active);

  sfu_subscriptions_snapshot_release(copy);
  assert(atomic_load(&b->refcount) == 2);
  sfu_subscriptions_snapshot_release(root);
  assert(atomic_load(&a->refcount) == 1);
  assert(atomic_load(&b->refcount) == 1);
  sfu_session_release(b);
  sfu_session_release(a);
}

static void test_membership_revision_and_capture_failure(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 15) == 0);
  sfu_peer_session_t *a = mock_session("revision-a");
  sfu_peer_session_t *b = mock_session("revision-b");

  assert(room.membership_revision == 0);
  assert(room_add_peer(&room, a));
  assert(room.membership_revision == 1);

  sfu_membership_event_test_fail_allocations(1);
  assert(!room_add_peer(&room, b));
  assert(room.peer_count == 1);
  assert(room.membership_revision == 1);
  assert(b->room == NULL);
  assert(receiver_count(a) == 0);

  assert(room_add_peer(&room, b));
  assert(room.membership_revision == 2);
  uint32_t a_high_water = sfu_session_remote_slot_high_water(a);
  uint32_t b_high_water = sfu_session_remote_slot_high_water(b);

  sfu_membership_event_test_fail_allocations(1);
  room_remove_peer(&room, b);
  assert(!atomic_load(&b->leave_event_in_use));
  sfu_membership_event_test_fail_allocations(0);
  assert(room.membership_revision == 3);
  assert(room.peer_count == 1);
  assert(a_high_water == 1);
  assert(b_high_water == 1);
  assert(receiver_count(a) == 0);

  room_remove_peer(&room, a);
  assert(room.membership_revision == 4);
  sfu_session_release(b);
  sfu_session_release(a);
  sfu_room_destroy(&room);
}

typedef struct room_add_race_ctx {
  sfu_room_t *room;
  sfu_peer_session_t *peer;
  pthread_barrier_t *barrier;
  bool added;
} room_add_race_ctx_t;

static void *room_add_race_thread(void *arg) {
  room_add_race_ctx_t *ctx = arg;
  pthread_barrier_wait(ctx->barrier);
  ctx->added = room_add_peer(ctx->room, ctx->peer);
  return NULL;
}

static void test_same_session_concurrent_room_add_has_one_winner(void) {
  sfu_room_t first, second;
  assert(sfu_room_init(&first, 17) == 0);
  assert(sfu_room_init(&second, 18) == 0);
  sfu_peer_session_t *peer = mock_session("two-room-race");
  pthread_barrier_t barrier;
  assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
  room_add_race_ctx_t a = {.room = &first, .peer = peer, .barrier = &barrier};
  room_add_race_ctx_t b = {.room = &second, .peer = peer, .barrier = &barrier};
  pthread_t ta, tb;
  assert(pthread_create(&ta, NULL, room_add_race_thread, &a) == 0);
  assert(pthread_create(&tb, NULL, room_add_race_thread, &b) == 0);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);
  assert(a.added != b.added);
  sfu_room_t *winner = a.added ? &first : &second;
  sfu_room_t *loser = a.added ? &second : &first;
  assert(peer->room == winner && winner->peer_count == 1 && winner->membership_revision == 1);
  assert(loser->peer_count == 0 && loser->membership_revision == 0 && loser->free_count == loser->peer_capacity);
  room_remove_peer(winner, peer);
  pthread_barrier_destroy(&barrier);
  sfu_session_release(peer);
  sfu_room_destroy(&second);
  sfu_room_destroy(&first);
}

static void *room_add_remove_once(void *arg) {
  room_add_race_ctx_t *ctx = arg;
  pthread_barrier_wait(ctx->barrier);
  assert(room_add_peer(ctx->room, ctx->peer));
  room_remove_peer(ctx->room, ctx->peer);
  return NULL;
}

static void test_concurrent_room_events_preserve_revision_order(void) {
  signaling_test_server.test_auto_drain = false;
  sfu_room_t room;
  assert(sfu_room_init(&room, 19) == 0);
  sfu_peer_session_t *a = mock_session("ordered-a");
  sfu_peer_session_t *b = mock_session("ordered-b");
  pthread_barrier_t barrier;
  assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
  room_add_race_ctx_t ca = {.room = &room, .peer = a, .barrier = &barrier};
  room_add_race_ctx_t cb = {.room = &room, .peer = b, .barrier = &barrier};
  pthread_t ta, tb;
  assert(pthread_create(&ta, NULL, room_add_remove_once, &ca) == 0);
  assert(pthread_create(&tb, NULL, room_add_remove_once, &cb) == 0);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);
  for (uint64_t revision = 1; revision <= 4; revision++) {
    sfu_membership_event_t *event = sfu_signaling_membership_test_pop(&signaling_test_server);
    assert(event != NULL);
    assert(event->room_id == room.room_id);
    assert(event->room_revision == revision);
    sfu_membership_event_release(event);
  }
  assert(sfu_signaling_membership_test_pop(&signaling_test_server) == NULL);
  signaling_test_server.test_auto_drain = true;
  pthread_barrier_destroy(&barrier);
  sfu_session_release(b);
  sfu_session_release(a);
  sfu_room_destroy(&room);
}

static void test_join_rejects_unavailable_signaling_transactionally(void) {
  sfu_signaling_membership_test_server_stop(&signaling_test_server);
  sfu_room_t room;
  assert(sfu_room_init(&room, 16) == 0);
  sfu_peer_session_t *peer = mock_session("no-signaling");
  uint32_t initial_refcount = atomic_load(&peer->refcount);
  uint32_t initial_high_water = sfu_session_remote_slot_high_water(peer);

  assert(!room_add_peer(&room, peer));
  assert(room.peer_count == 0);
  assert(room.membership_revision == 0);
  assert(peer->room == NULL);
  assert(peer->room_slot == UINT32_MAX);
  assert(receiver_count(peer) == 0);
  assert(fanout_target_count(peer) == 0);
  assert(sfu_session_remote_slot_high_water(peer) == initial_high_water);
  assert(atomic_load(&peer->refcount) == initial_refcount);

  sfu_session_release(peer);
  sfu_room_destroy(&room);
  sfu_signaling_membership_test_server_init(&signaling_test_server);
  signaling_test_server.test_auto_drain = true;
}

static void test_remote_slot_reuse_over_1000_churn(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 20) == 0);
  sfu_peer_session_t *stable = mock_session("slot-reuse-stable");
  sfu_peer_session_t *flapper = mock_session("slot-reuse-flapper");
  assert(room_add_peer(&room, stable));

  uint64_t previous_generation = 0;
  for (uint32_t i = 0; i < 1200; i++) {
    assert(room_add_peer(&room, flapper));
    sfu_receiver_snapshot_t *snapshot = sfu_session_subscriptions_acquire(stable);
    assert(snapshot != NULL && snapshot->count == 1);
    const sfu_receiver_entry_t *entry = sfu_receiver_snapshot_nth(snapshot, 0, NULL);
    assert(entry != NULL && entry->remote_slot == 0 && entry->assignment_generation != 0);
    assert(entry->assignment_generation != previous_generation);
    previous_generation = entry->assignment_generation;
    sfu_subscriptions_snapshot_release(snapshot);
    room_remove_peer(&room, flapper);
    assert(receiver_count(stable) == 0);
    assert(sfu_session_remote_slot_high_water(stable) == 1);
  }

  room_remove_peer(&room, stable);
  sfu_session_release(flapper);
  sfu_session_release(stable);
  sfu_room_destroy(&room);
}

static void test_audience_join_renegotiation_suppression(void) {
  signaling_test_server.test_auto_drain = false;
  sfu_room_t room;
  assert(sfu_room_init(&room, 8) == 0);

  sfu_peer_session_t *speaker_a = mock_session("spk-a");
  speaker_a->peer_id = 101;
  speaker_a->user_id = 1001;
  speaker_a->media.uplink_audio.ssrc = 1111;
  speaker_a->media.uplink_audio.active = true;
  speaker_a->media.uplink_video.ssrc = 2222;
  speaker_a->media.uplink_video.rtx_ssrc = 3333;
  speaker_a->media.uplink_video.payload_type = 96;
  speaker_a->media.uplink_video.rtx_payload_type = 97;
  speaker_a->media.uplink_video.codec = SFU_VIDEO_CODEC_VP8;
  speaker_a->media.uplink_video.active = true;

  sfu_peer_session_t *audience_b = mock_session("aud-b");
  audience_b->peer_id = 102;
  audience_b->user_id = 1002;
  audience_b->media.uplink_audio.ssrc = 7777;
  audience_b->media.uplink_audio.active = false;
  audience_b->media.uplink_video.active = false;
  atomic_store(&audience_b->media.audio_send_negotiated, true);
  atomic_store(&audience_b->is_audience, true);

  sfu_peer_session_t *speaker_c = mock_session("spk-c");
  speaker_c->peer_id = 103;
  speaker_c->user_id = 1003;
  speaker_c->media.uplink_audio.ssrc = 4444;
  speaker_c->media.uplink_audio.active = true;

  assert(room_add_peer(&room, speaker_a));
  sfu_membership_event_t *event_a = sfu_signaling_membership_test_pop(&signaling_test_server);
  assert(event_a != NULL);
  assert(event_a->kind == SFU_MEMBERSHIP_JOIN);
  assert(event_a->room_revision == 1);
  assert(event_a->participant_count == 1);
  assert(event_a->subject_peer_id == speaker_a->peer_id);
  assert(!event_a->subject_is_audience);
  assert(event_a->recipient_count == 1);
  assert(event_a->recipients[0].session == speaker_a);
  assert(event_a->recipients[0].send_snapshot);
  assert(!event_a->recipients[0].send_delta);
  assert(!event_a->recipients[0].renegotiate);
  assert(event_a->recipients[0].remote_slot == UINT32_MAX);
  assert(event_a->member_count == 1);
  assert(event_a->members[0].peer_id == speaker_a->peer_id);
  assert(!event_a->members[0].is_audience);
  sfu_membership_event_release(event_a);

  assert(room_add_peer(&room, audience_b));
  sfu_membership_event_t *event_b = sfu_signaling_membership_test_pop(&signaling_test_server);
  assert(event_b != NULL);
  assert(event_b->kind == SFU_MEMBERSHIP_JOIN);
  assert(event_b->room_revision == 2);
  assert(event_b->participant_count == 2);
  assert(event_b->subject_peer_id == audience_b->peer_id);
  assert(event_b->subject_is_audience);
  assert(event_b->recipient_count == 2);
  assert(event_b->recipients[0].session == audience_b);
  assert(event_b->recipients[0].send_snapshot);
  assert(!event_b->recipients[0].send_delta);
  assert(event_b->recipients[0].renegotiate);
  assert(event_b->recipients[0].remote_slot == UINT32_MAX);
  assert(event_b->recipients[1].session == speaker_a);
  assert(!event_b->recipients[1].send_snapshot);
  assert(event_b->recipients[1].send_delta);
  assert(!event_b->recipients[1].renegotiate);
  assert(event_b->recipients[1].remote_slot != UINT32_MAX);
  assert(event_b->recipients[1].mid_audio == sfu_remote_slot_first_mid(event_b->recipients[1].remote_slot));
  assert(event_b->recipients[1].mid_video == event_b->recipients[1].mid_audio + 1);
  assert(event_b->recipients[1].mid_screen == event_b->recipients[1].mid_audio + 2);
  assert(event_b->member_count == 2);
  assert(event_b->members[0].peer_id == audience_b->peer_id);
  assert(event_b->members[0].is_audience);
  assert(event_b->members[1].peer_id == speaker_a->peer_id);
  assert(!event_b->members[1].is_audience);
  assert(event_b->members[1].remote_slot == 0);
  assert(event_b->members[1].mid_audio == SFU_REMOTE_MID_BASE);
  sfu_membership_event_release(event_b);

  assert(room_add_peer(&room, speaker_c));
  sfu_membership_event_t *event_c = sfu_signaling_membership_test_pop(&signaling_test_server);
  assert(event_c != NULL);
  assert(event_c->kind == SFU_MEMBERSHIP_JOIN);
  assert(event_c->room_revision == 3);
  assert(event_c->participant_count == 3);
  assert(event_c->subject_peer_id == speaker_c->peer_id);
  assert(!event_c->subject_is_audience);
  assert(event_c->recipient_count == 3);
  assert(event_c->recipients[0].session == speaker_c);
  assert(event_c->recipients[0].send_snapshot);
  assert(!event_c->recipients[0].send_delta);
  assert(event_c->recipients[0].renegotiate);
  for (uint32_t i = 1; i < event_c->recipient_count; i++) {
    assert(event_c->recipients[i].send_delta);
    assert(event_c->recipients[i].renegotiate);
    assert(event_c->recipients[i].remote_slot != UINT32_MAX);
    assert(event_c->recipients[i].mid_audio == sfu_remote_slot_first_mid(event_c->recipients[i].remote_slot));
  }
  assert(event_c->member_count == 3);
  sfu_membership_event_release(event_c);

  assert(room_set_peer_ptt_active(&room, audience_b, true));
  assert(atomic_load(&audience_b->media.ptt_active));
  assert(audio_route_count(audience_b) == 2);
  assert(room_set_peer_ptt_active(&room, audience_b, false));
  assert(!atomic_load(&audience_b->media.ptt_active));
  assert(audio_route_count(audience_b) == 0);

  assert(room_update_peer_role(&room, audience_b, false));
  assert(!atomic_load(&audience_b->is_audience));
  audience_b->media.uplink_audio.ssrc = 7777;
  audience_b->media.uplink_audio.active = true;
  room_refresh_peer_streams(&room, audience_b);
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker_a);
    assert(snap != NULL);
    const sfu_receiver_entry_t *entry = sfu_receiver_snapshot_find_peer(snap, audience_b, NULL);
    assert(entry != NULL && entry->audio_active && entry->audio_ssrc == 7777);
    sfu_subscriptions_snapshot_release(snap);
  }

  assert(room_update_peer_role(&room, audience_b, true));
  assert(atomic_load(&audience_b->is_audience));
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker_a);
    assert(snap != NULL);
    const sfu_receiver_entry_t *entry = sfu_receiver_snapshot_find_peer(snap, audience_b, NULL);
    assert(entry != NULL && !entry->audio_active);
    sfu_subscriptions_snapshot_release(snap);
  }

  room_remove_peer(&room, speaker_c);
  sfu_membership_event_t *leave_c = sfu_signaling_membership_test_pop(&signaling_test_server);
  if (leave_c) sfu_membership_event_release(leave_c);
  room_remove_peer(&room, audience_b);
  sfu_membership_event_t *leave_b = sfu_signaling_membership_test_pop(&signaling_test_server);
  if (leave_b) sfu_membership_event_release(leave_b);
  room_remove_peer(&room, speaker_a);
  sfu_membership_event_t *leave_a = sfu_signaling_membership_test_pop(&signaling_test_server);
  if (leave_a) sfu_membership_event_release(leave_a);

  assert(sfu_signaling_membership_test_pop(&signaling_test_server) == NULL);
  signaling_test_server.test_auto_drain = true;

  sfu_session_release(speaker_c);
  sfu_session_release(audience_b);
  sfu_session_release(speaker_a);
  sfu_room_destroy(&room);
}

int main(void) {
  sfu_signaling_membership_test_server_init(&signaling_test_server);
  signaling_test_server.test_auto_drain = true;
  test_membership_revision_and_capture_failure();
  test_chunked_subscription_root_copy();
  test_add_remove();
  test_audience_role_asymmetry_and_transition();
  test_audience_join_renegotiation_suppression();
  test_snapshot_hold_across_replace();
  test_concurrent_snapshot_read_write();
  test_concurrent_fanout_bundle_acquire_iterate_churn();
  test_multi_publisher_concurrent_churn();
  test_fanout_uses_publisher_stream_identity();
  test_three_peer_route_mids();
  test_add_rejects_exhausted_mid_capacity_transactionally();
  test_removed_peer_refresh_is_noop();
  test_same_session_concurrent_room_add_has_one_winner();
  test_concurrent_room_events_preserve_revision_order();
  test_join_rejects_unavailable_signaling_transactionally();
  test_remote_slot_reuse_over_1000_churn();

  sfu_signaling_membership_test_server_stop(&signaling_test_server);
  printf("test_room: OK\n");
  return 0;
}
