#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "peer/session.h"
#include "room/room.h"
#include "room/room_media_graph.h"
#include "util/alloc.h"

/* Heap-allocated refcounted mock session (no DTLS/RTX; construction bypasses
 * the session table because these tests exercise the room graph, not DTLS). */
static sfu_peer_session_t *mock_session(const char *ufrag) {
  sfu_peer_session_t *s = SFU_CALLOC(1, sizeof(*s));
  assert(s != NULL);
  s->cold = SFU_CALLOC(1, sizeof(*s->cold));
  assert(s->cold != NULL);
  snprintf(s->cold->ufrag, sizeof(s->cold->ufrag), "%s", ufrag);
  s->active = true;
  assert(pthread_mutex_init(&s->answer_lock, NULL) == 0);
  assert(pthread_mutex_init(&s->negotiation.lock, NULL) == 0);
  assert(pthread_mutex_init(&s->media_lock, NULL) == 0);
  assert(pthread_mutex_init(&s->snapshot_lock, NULL) == 0);
  atomic_store(&s->refcount, 1);
  atomic_store(&s->lifecycle, SFU_SESSION_LIFECYCLE_OPEN);
  atomic_store(&s->accepts_work, true);
  atomic_store(&s->visible, true);
  s->uplink_audio.owner = s;
  s->uplink_video.owner = s;
  s->uplink_audio.active = true;
  s->uplink_video.active = true;
  s->next_remote_mid = SFU_REMOTE_MID_BASE;
  return s;
}

static uint32_t receiver_count(sfu_peer_session_t *peer) {
  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(peer);
  uint32_t n = snap ? snap->count : 0;
  sfu_subscriptions_snapshot_release(snap);
  return n;
}

static uint32_t fanout_target_count(sfu_peer_session_t *peer) {
  sfu_receiver_snapshot_t *snap = sfu_session_fanout_targets_acquire(peer);
  uint32_t n = snap ? snap->count : 0;
  sfu_subscriptions_snapshot_release(snap);
  return n;
}

static bool fanout_targets(sfu_peer_session_t *peer, sfu_peer_session_t *dest) {
  sfu_receiver_snapshot_t *snap = sfu_session_fanout_targets_acquire(peer);
  bool found = false;
  if (snap) {
    for (uint32_t i = 0; i < snap->count; i++) {
      if (snap->entries[i].subscriber == dest) {
        found = true;
        break;
      }
    }
  }
  sfu_subscriptions_snapshot_release(snap);
  return found;
}

static uint32_t audio_route_count(sfu_peer_session_t *peer) {
  sfu_audio_route_snapshot_t *snap = sfu_session_audio_fanout_acquire(peer);
  uint32_t n = snap ? snap->count : 0;
  sfu_audio_route_snapshot_release(snap);
  return n;
}

static uint32_t video_route_count(sfu_peer_session_t *peer) {
  sfu_video_route_snapshot_t *snap = sfu_session_video_fanout_acquire(peer);
  uint32_t n = snap ? snap->count : 0;
  sfu_video_route_snapshot_release(snap);
  return n;
}

static bool subscribes_to(sfu_peer_session_t *peer, sfu_peer_session_t *dest) {
  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(peer);
  bool found = false;
  if (snap) {
    for (uint32_t i = 0; i < snap->count; i++) {
      if (snap->entries[i].subscriber == dest) {
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
  assert(receiver_count(a) == 0);

  room_add_peer(&room, b);
  assert(receiver_count(a) == 1);
  assert(receiver_count(b) == 1);
  assert(subscribes_to(a, b));
  assert(subscribes_to(b, a));
  assert(audio_route_count(a) == 1 && audio_route_count(b) == 1);
  assert(video_route_count(a) == 1 && video_route_count(b) == 1);

  b->uplink_video.active = false;
  room_refresh_peer_streams(&room, b);
  assert(receiver_count(a) == 1); /* stable SDP slot remains */
  assert(video_route_count(b) == 0); /* inactive source omitted from hot path */
  assert(audio_route_count(b) == 1);
  b->uplink_video.active = true;
  room_refresh_peer_streams(&room, b);
  assert(video_route_count(b) == 1);

  room_add_peer(&room, c);
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
    for (uint32_t i = 0; i < snap->count; i++) {
      if (snap->entries[i].subscriber == c) {
        a_mid_for_c_audio = snap->entries[i].mid_audio;
        a_mid_for_c_video = snap->entries[i].mid_video;
      }
    }
    sfu_subscriptions_snapshot_release(snap);
  }
  assert(a_mid_for_c_audio != 0 || a_mid_for_c_video != 0);

  room_remove_peer(&room, b);
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
    assert(snap->entries[0].subscriber == c);
    assert(snap->entries[0].mid_audio == a_mid_for_c_audio);
    assert(snap->entries[0].mid_video == a_mid_for_c_video);
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
  /* re-add is a no-op: b is still a member from the rejoin above */
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
  speaker->uplink_audio.ssrc = 1111;
  speaker->uplink_video.ssrc = 2222;
  speaker->uplink_video.rtx_ssrc = 3333;
  speaker->uplink_video.payload_type = 96;
  speaker->uplink_video.rtx_payload_type = 97;
  speaker->uplink_video.codec = SFU_VIDEO_CODEC_VP8;
  sfu_peer_session_t *audience = mock_session("audience");
  audience->screen.ssrc = 4444;
  audience->screen.rtx_ssrc = 5555;
  audience->screen.payload_type = 96;
  audience->screen.rtx_payload_type = 97;
  audience->screen.codec = SFU_VIDEO_CODEC_VP8;
  audience->screen.active = false;
  audience->uplink_audio.ssrc = 7777;
  audience->uplink_audio.active = false;
  audience->uplink_video.active = false;
  atomic_store(&audience->audio_send_negotiated, true);
  atomic_store(&audience->is_audience, true);

  room_add_peer(&room, speaker);
  room_add_peer(&room, audience);

  /* Audience receives the speaker SDP source but never owns an RTP fanout
   * snapshot. The speaker forwards to the audience. */
  assert(receiver_count(audience) == 1);
  assert(audience->next_remote_mid == SFU_REMOTE_MID_BASE + SFU_REMOTE_TRANSCEIVERS_PER_SLOT);
  assert(subscribes_to(audience, speaker));
  assert(receiver_count(speaker) == 1);
  assert(subscribes_to(speaker, audience));
  assert(fanout_target_count(speaker) == 1);
  assert(fanout_targets(speaker, audience));
  assert(fanout_target_count(audience) == 1);
  assert(fanout_targets(audience, speaker));
  uint32_t audience_mid_before_ptt = 0;
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    audience_mid_before_ptt = snap->entries[0].mid_audio;
    assert(snap->entries[0].has_audio);
    assert(!snap->entries[0].has_video);
    assert(!snap->entries[0].has_screen);
    assert(!snap->entries[0].audio_active);
    sfu_subscriptions_snapshot_release(snap);
  }
  uint32_t speaker_next_mid_before_ptt = speaker->next_remote_mid;
  assert(audio_route_count(audience) == 0);
  assert(room_set_peer_ptt_active(&room, audience, true));
  assert(atomic_load(&audience->is_audience));
  assert(atomic_load(&audience->ptt_active));
  assert(audio_route_count(audience) == 1);
  assert(speaker->next_remote_mid == speaker_next_mid_before_ptt);
  assert(room_set_peer_ptt_active(&room, audience, false));
  assert(!atomic_load(&audience->ptt_active));
  assert(audio_route_count(audience) == 0);
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    assert(snap->entries[0].mid_audio == audience_mid_before_ptt);
    assert(!snap->entries[0].audio_active);
    sfu_subscriptions_snapshot_release(snap);
  }
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(audience);
    assert(snap != NULL && snap->count == 1);
    assert(snap->entries[0].subscriber == speaker);
    assert(snap->entries[0].mid_audio == SFU_REMOTE_MID_BASE);
    assert(snap->entries[0].mid_video == SFU_REMOTE_MID_BASE + 1);
    assert(snap->entries[0].mid_screen == SFU_REMOTE_MID_BASE + 2);
    assert(snap->entries[0].audio_active);
    assert(snap->entries[0].video_active);
    assert(snap->entries[0].audio_ssrc == 1111);
    assert(snap->entries[0].video_ssrc == 2222);
    assert(snap->entries[0].video_rtx_ssrc == 3333);
    assert(snap->entries[0].video_pt == 96);
    assert(snap->entries[0].video_rtx_pt == 97);
    assert(snap->entries[0].video_codec == SFU_VIDEO_CODEC_VP8);
    sfu_subscriptions_snapshot_release(snap);
  }

  assert(room_update_peer_role(&room, audience, false));
  assert(!atomic_load(&audience->is_audience));
  assert(receiver_count(speaker) == 1);
  assert(subscribes_to(speaker, audience));
  assert(fanout_target_count(audience) == 1);
  assert(fanout_targets(audience, speaker));

  assert(room_update_peer_role(&room, audience, true));
  assert(atomic_load(&audience->is_audience));
  /* Demotion keeps the subscription slot (deactivated) so its mids stay
   * stable for a later re-promotion; the audience no longer owns fanout
   * targets but the speaker still receives from it. */
  assert(receiver_count(speaker) == 1);
  assert(fanout_target_count(audience) == 1);
  assert(fanout_targets(speaker, audience));

  /* The deactivated slot has its media state cleared... */
  uint32_t mid_audio = 0, mid_video = 0, mid_screen = 0;
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    assert(snap->entries[0].subscriber == audience);
    assert(!snap->entries[0].audio_active);
    assert(!snap->entries[0].video_active);
    assert(!snap->entries[0].screen_active);
    assert(snap->entries[0].audio_ssrc == 0);
    assert(snap->entries[0].video_ssrc == 0);
    assert(snap->entries[0].screen_ssrc == 0);
    mid_audio = snap->entries[0].mid_audio;
    mid_video = snap->entries[0].mid_video;
    mid_screen = snap->entries[0].mid_screen;
    sfu_subscriptions_snapshot_release(snap);
  }
  /* ...and the demoted peer's uplink state is reset so a re-promotion with
   * fresh browser SSRCs is learned from RTP again. */
  assert(audience->uplink_audio.ssrc == 0);
  assert(!audience->uplink_audio.active);
  assert(audience->uplink_video.ssrc == 0);
  assert(!audience->uplink_video.active);
  assert(audience->screen.ssrc == 0);
  assert(!audience->screen.active);

  /* Re-promotion with fresh uplink SSRCs reuses the same slot: mids are
   * preserved (the subscriber's transceivers stay bound) while the new
   * media state is picked up. */
  audience->uplink_audio.ssrc = 1111;
  audience->uplink_audio.active = true;
  audience->uplink_video.ssrc = 2222;
  audience->uplink_video.rtx_ssrc = 3333;
  audience->uplink_video.active = true;
  audience->screen.ssrc = 4444;
  audience->screen.rtx_ssrc = 5555;
  audience->screen.payload_type = 96;
  audience->screen.rtx_payload_type = 97;
  audience->screen.codec = SFU_VIDEO_CODEC_VP8;
  audience->screen.active = true;

  uint32_t speaker_next_mid = speaker->next_remote_mid;
  assert(room_update_peer_role(&room, audience, false));
  assert(receiver_count(speaker) == 1);
  assert(fanout_targets(audience, speaker));
  {
    sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(speaker);
    assert(snap != NULL && snap->count == 1);
    assert(snap->entries[0].subscriber == audience);
    assert(snap->entries[0].mid_audio == mid_audio);
    assert(snap->entries[0].mid_video == mid_video);
    assert(snap->entries[0].mid_screen == mid_screen);
    assert(snap->entries[0].audio_active);
    assert(snap->entries[0].video_active);
    assert(snap->entries[0].screen_active);
    assert(snap->entries[0].audio_ssrc == 1111);
    assert(snap->entries[0].video_ssrc == 2222);
    assert(snap->entries[0].video_rtx_ssrc == 3333);
    assert(snap->entries[0].screen_ssrc == 4444);
    assert(snap->entries[0].screen_rtx_ssrc == 5555);
    sfu_subscriptions_snapshot_release(snap);
  }
  /* No fresh mid pair was allocated for the re-promoted peer. */
  assert(speaker->next_remote_mid == speaker_next_mid);

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
  assert(held->entries[0].subscriber == b);

  /* Writer publishes a replacement while we hold the old one. */
  room_add_peer(&room, c);

  /* The held (old) snapshot is unchanged and its entries are still valid. */
  assert(held->count == 1);
  assert(held->entries[0].subscriber == b);
  assert(held->entries[0].subscriber->cold != NULL);

  /* New acquisitions see the replacement. */
  sfu_receiver_snapshot_t *fresh = sfu_session_subscriptions_acquire(a);
  assert(fresh != NULL && fresh->count == 2);
  assert(fresh != held);
  assert(fresh->generation == held->generation + 1);
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
      for (uint32_t e = 0; e < snap->count; e++) {
        /* Entries must be coherent: retained subscriber with valid cold. */
        assert(snap->entries[e].subscriber != NULL);
        assert(snap->entries[e].subscriber->cold != NULL);
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
      for (uint32_t e = 0; e < snap->count; e++) {
        /* Coherence: retained subscriber with valid cold, even mid-churn. */
        assert(snap->entries[e].subscriber != NULL);
        assert(snap->entries[e].subscriber->cold != NULL);
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
  publisher->uplink_video.ssrc = 1111;
  publisher->uplink_video.rtx_ssrc = 2222;
  publisher->uplink_video.payload_type = 98;
  publisher->uplink_video.rtx_payload_type = 97;
  publisher->uplink_video.codec = SFU_VIDEO_CODEC_VP9;
  subscriber->uplink_video.ssrc = 3333;
  subscriber->uplink_video.rtx_ssrc = 4444;
  subscriber->uplink_video.payload_type = 96;
  subscriber->uplink_video.rtx_payload_type = 99;
  subscriber->uplink_video.codec = SFU_VIDEO_CODEC_VP8;

  room_add_peer(&room, publisher);
  room_add_peer(&room, subscriber);

  sfu_receiver_snapshot_t *snap = sfu_session_fanout_targets_acquire(publisher);
  assert(snap != NULL && snap->count == 1);
  const sfu_receiver_entry_t *entry = &snap->entries[0];
  assert(entry->subscriber == subscriber);
  assert(entry->video_ssrc == publisher->uplink_video.ssrc);
  assert(entry->video_rtx_ssrc == publisher->uplink_video.rtx_ssrc);
  assert(entry->mid_audio == SFU_REMOTE_MID_BASE);
  assert(entry->mid_video == SFU_REMOTE_MID_BASE + 1);
  assert(entry->mid_screen == SFU_REMOTE_MID_BASE + 2);
  assert(entry->video_pt == publisher->uplink_video.payload_type);
  assert(entry->video_rtx_pt == publisher->uplink_video.rtx_payload_type);
  assert(entry->video_codec == SFU_VIDEO_CODEC_VP9);
  assert(strcmp(entry->subscriber_ufrag, publisher->cold->ufrag) == 0);
  sfu_subscriptions_snapshot_release(snap);

  snap = sfu_session_subscriptions_acquire(subscriber);
  assert(snap != NULL && snap->count == 1);
  entry = &snap->entries[0];
  assert(entry->subscriber == publisher);
  assert(entry->video_ssrc == publisher->uplink_video.ssrc);
  assert(entry->video_codec == SFU_VIDEO_CODEC_VP9);
  sfu_subscriptions_snapshot_release(snap);

  room_remove_peer(&room, subscriber);
  room_remove_peer(&room, publisher);
  sfu_session_release(subscriber);
  sfu_session_release(publisher);
  sfu_room_destroy(&room);
}

static void assert_fanout_mids_match_subscriptions(sfu_peer_session_t *publisher, uint32_t expected_targets) {
  sfu_receiver_snapshot_t *fanout = sfu_session_fanout_targets_acquire(publisher);
  assert(fanout != NULL && fanout->count == expected_targets);
  for (uint32_t i = 0; i < fanout->count; i++) {
    const sfu_receiver_entry_t *route = &fanout->entries[i];
    sfu_receiver_snapshot_t *subscriptions = sfu_session_subscriptions_acquire(route->subscriber);
    uint32_t pos = UINT32_MAX;
    for (uint32_t j = 0; subscriptions && j < subscriptions->count; j++) {
      if (subscriptions->entries[j].subscriber == publisher) {
        pos = j;
        break;
      }
    }
    assert(pos != UINT32_MAX);
    assert(route->mid_audio == subscriptions->entries[pos].mid_audio);
    assert(route->mid_video == subscriptions->entries[pos].mid_video);
    assert(route->mid_screen == subscriptions->entries[pos].mid_screen);
    assert(route->mid_audio >= SFU_REMOTE_MID_BASE);
    assert(route->mid_video == route->mid_audio + 1);
    assert(route->mid_screen == route->mid_audio + 2);
    sfu_subscriptions_snapshot_release(subscriptions);
  }
  sfu_subscriptions_snapshot_release(fanout);
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
  assert(a->next_remote_mid == SFU_REMOTE_MID_BASE + 2 * SFU_REMOTE_TRANSCEIVERS_PER_SLOT);
  assert(b->next_remote_mid == SFU_REMOTE_MID_BASE + 2 * SFU_REMOTE_TRANSCEIVERS_PER_SLOT);
  assert(c->next_remote_mid == SFU_REMOTE_MID_BASE + 2 * SFU_REMOTE_TRANSCEIVERS_PER_SLOT);

  room_remove_peer(&room, c);
  room_remove_peer(&room, b);
  room_remove_peer(&room, a);
  sfu_session_release(c);
  sfu_session_release(b);
  sfu_session_release(a);
  sfu_room_destroy(&room);
}

int main(void) {
  test_add_remove();
  test_audience_role_asymmetry_and_transition();
  test_snapshot_hold_across_replace();
  test_concurrent_snapshot_read_write();
  test_multi_publisher_concurrent_churn();
  test_fanout_uses_publisher_stream_identity();
  test_three_peer_route_mids();

  printf("test_room: OK\n");
  return 0;
}
