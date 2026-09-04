#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "peer/session.h"
#include "protocol/signaling/signaling.h"

static void test_screen_codec_preference_parsing(void) {
  const char *vp9 = "{\"type\":\"join\",\"screen_codec\":\"vp9\"}";
  const char *vp8 = "{\"screen_codec\":\"VP8\"}";
  const char *legacy = "{\"type\":\"join\"}";
  const char *invalid = "{\"screen_codec\":\"h264\"}";
  assert(sfu_signaling_parse_screen_codec_preference(vp9, strlen(vp9)) == SFU_VIDEO_CODEC_VP9);
  assert(sfu_signaling_parse_screen_codec_preference(vp8, strlen(vp8)) == SFU_VIDEO_CODEC_VP8);
  assert(sfu_signaling_parse_screen_codec_preference(legacy, strlen(legacy)) == SFU_VIDEO_CODEC_NONE);
  assert(sfu_signaling_parse_screen_codec_preference(invalid, strlen(invalid)) == SFU_VIDEO_CODEC_NONE);
}

static void test_join_capture_failure_is_reported(void) {
  sfu_membership_event_test_fail_allocations(1);
  assert(sfu_membership_event_alloc() == NULL);
}

static void test_queue_releases_when_signaling_is_stopped(void) {
  sfu_membership_event_t *event = sfu_membership_event_alloc();
  assert(event != NULL);
  event->kind = SFU_MEMBERSHIP_JOIN;
  event->room_id = 42;
  event->room_revision = 9;
  assert(!sfu_signaling_queue_membership_event(event));
}

static void test_preallocated_leave_event_released_on_stopped_queue(void) {
  sfu_peer_session_t owner = {0};
  sfu_membership_event_t event = {0};
  atomic_store(&owner.refcount, 2);
  atomic_store(&owner.leave_event_in_use, true);
  owner.leave_event = &event;
  event.kind = SFU_MEMBERSHIP_LEAVE;
  event.preallocated_storage = true;
  event.storage_owner = &owner;
  event.room_id = 77;
  event.room_revision = 4;

  assert(!sfu_signaling_queue_membership_event(&event));
  assert(!atomic_load(&owner.leave_event_in_use));
  assert(atomic_load(&owner.refcount) == 1);
  assert(event.preallocated_storage);
  assert(event.storage_owner == &owner);
  assert(event.room_id == 0);
  assert(event.room_revision == 0);
}

typedef struct enqueue_ctx {
  sfu_membership_event_t *event;
  atomic_bool entered;
  atomic_bool completed;
  bool result;
} enqueue_ctx_t;

static void *enqueue_thread(void *arg) {
  enqueue_ctx_t *ctx = arg;
  atomic_store(&ctx->entered, true);
  ctx->result = sfu_signaling_queue_membership_event(ctx->event);
  atomic_store(&ctx->completed, true);
  return NULL;
}

static void test_membership_queue_blocks_until_capacity_returns(void) {
  sfu_signaling_server_t server;
  sfu_signaling_membership_test_server_init(&server);
  pthread_mutex_lock(&server.membership_queue.lock);
  server.membership_queue.count = SFU_MEMBERSHIP_QUEUE_CAP;
  pthread_mutex_unlock(&server.membership_queue.lock);

  enqueue_ctx_t ctx = {.event = sfu_membership_event_alloc()};
  assert(ctx.event != NULL);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, enqueue_thread, &ctx) == 0);
  while (!atomic_load(&ctx.entered)) {
  }
  for (uint32_t i = 0; i < 10000 && !atomic_load(&ctx.completed); i++) {
    sched_yield();
  }
  assert(!atomic_load(&ctx.completed));

  pthread_mutex_lock(&server.membership_queue.lock);
  server.membership_queue.count--;
  pthread_cond_signal(&server.membership_queue.not_full);
  pthread_mutex_unlock(&server.membership_queue.lock);
  pthread_join(thread, NULL);
  assert(ctx.result);
  assert(sfu_signaling_membership_test_pop(&server) == ctx.event);
  sfu_membership_event_release(ctx.event);
  pthread_mutex_lock(&server.membership_queue.lock);
  server.membership_queue.count = 0;
  pthread_mutex_unlock(&server.membership_queue.lock);
  sfu_signaling_membership_test_server_stop(&server);
}

static void test_shutdown_wakes_blocked_membership_producer(void) {
  sfu_signaling_server_t server;
  sfu_signaling_membership_test_server_init(&server);
  pthread_mutex_lock(&server.membership_queue.lock);
  server.membership_queue.count = SFU_MEMBERSHIP_QUEUE_CAP;
  pthread_mutex_unlock(&server.membership_queue.lock);

  enqueue_ctx_t ctx = {.event = sfu_membership_event_alloc()};
  assert(ctx.event != NULL);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, enqueue_thread, &ctx) == 0);
  while (!atomic_load(&ctx.entered)) {
  }
  sfu_signaling_membership_test_server_stop(&server);
  pthread_join(thread, NULL);
  assert(atomic_load(&ctx.completed));
  assert(!ctx.result);
}

static void init_renegotiation_peer(sfu_peer_session_t *peer) {
  memset(peer, 0, sizeof(*peer));
  atomic_store(&peer->refcount, 1);
  atomic_store(&peer->accepts_work, true);
  peer->state = SFU_SESSION_ESTABLISHED;
  peer->fd = 7;
  assert(pthread_mutex_init(&peer->graph.lock, NULL) == 0);
  assert(pthread_mutex_init(&peer->negotiation.lock, NULL) == 0);
}

static void destroy_renegotiation_peer(sfu_peer_session_t *peer) {
  assert(atomic_load(&peer->refcount) == 1);
  sfu_session_remote_slots_teardown(peer);
  pthread_mutex_destroy(&peer->negotiation.lock);
  pthread_mutex_destroy(&peer->graph.lock);
}

static void release_test_queue_reference(sfu_peer_session_t *peer) { assert(atomic_fetch_sub(&peer->refcount, 1) > 1); }

static void test_remote_slot_reconciliation_is_idempotent(void) {
  sfu_signaling_server_t server;
  sfu_signaling_renegotiation_test_server_init(&server);
  sfu_peer_session_t peer;
  init_renegotiation_peer(&peer);

  uint32_t slot;
  uint64_t generation;
  assert(sfu_session_remote_slot_reserve(&peer, 4001, 41, &slot, &generation));
  assert(sfu_signaling_reconcile_remote_slots(&peer));
  assert(sfu_signaling_reconcile_remote_slots(&peer));
  assert(peer.negotiation.desired_offer_revision == 1);
  assert(peer.negotiation.renegotiation_pending);
  assert(peer.negotiation.negotiation_needed);
  assert(sfu_signaling_renegotiation_test_count(&server) == 1);
  assert(sfu_signaling_renegotiation_test_pop(&server) == &peer);
  release_test_queue_reference(&peer);

  peer.negotiation.negotiation_needed = false;
  peer.negotiation.offer_outstanding = true;
  peer.negotiation.answered_revision = 1;
  peer.negotiation.desired_offer_revision = 1;
  assert(sfu_signaling_reconcile_remote_slots(&peer));
  assert(sfu_signaling_reconcile_remote_slots(&peer));
  assert(peer.negotiation.desired_offer_revision == 2);
  assert(sfu_signaling_renegotiation_test_count(&server) == 0);

  peer.negotiation.offer_outstanding = false;
  assert(sfu_signaling_reconcile_remote_slots(&peer));
  assert(peer.negotiation.desired_offer_revision == 2);
  assert(sfu_signaling_renegotiation_test_count(&server) == 1);
  assert(sfu_signaling_renegotiation_test_pop(&server) == &peer);
  release_test_queue_reference(&peer);
  peer.negotiation.negotiation_needed = false;

  destroy_renegotiation_peer(&peer);
  sfu_signaling_renegotiation_test_server_stop(&server);
}

static void test_remote_slot_follow_up_captures_fresh_manifest(void) {
  sfu_peer_session_t peer;
  init_renegotiation_peer(&peer);

  uint32_t old_slot;
  uint64_t old_generation;
  assert(sfu_session_remote_slot_reserve(&peer, 4001, 41, &old_slot, &old_generation));
  sfu_remote_offer_manifest_t *first = sfu_signaling_capture_offer_manifest(&peer);
  assert(first != NULL);
  uint64_t first_offer_generation = first->offer_generation;
  assert(first_offer_generation != 0);
  assert(first->assignment_generations[old_slot] == old_generation);
  assert(sfu_session_remote_offer_install(&peer, first));

  uint32_t replacement_slot;
  uint64_t replacement_generation;
  assert(sfu_session_remote_slot_reserve(&peer, 4002, 42, &replacement_slot, &replacement_generation));
  assert(!sfu_session_remote_slot_authorized(&peer, replacement_slot, replacement_generation));
  assert(sfu_session_remote_offer_apply_answer(&peer, first));
  sfu_remote_offer_manifest_release(first);

  sfu_remote_offer_manifest_t *follow_up = sfu_signaling_capture_offer_manifest(&peer);
  assert(follow_up != NULL);
  assert(follow_up->offer_generation > first_offer_generation);
  assert(follow_up->assignment_generations[replacement_slot] == replacement_generation);
  assert(sfu_session_remote_offer_install(&peer, follow_up));
  assert(sfu_session_remote_offer_apply_answer(&peer, follow_up));
  sfu_remote_offer_manifest_release(follow_up);
  assert(sfu_session_remote_slot_authorized(&peer, replacement_slot, replacement_generation));
  assert(!sfu_session_remote_slots_pending(&peer, NULL, NULL));

  destroy_renegotiation_peer(&peer);
}

static void test_renegotiation_queue_reclaims_closing_sessions(void) {
  sfu_signaling_server_t server;
  sfu_signaling_renegotiation_test_server_init(&server);
  sfu_peer_session_t stale;
  sfu_peer_session_t replacements[3];
  init_renegotiation_peer(&stale);
  atomic_store(&stale.accepts_work, false);
  stale.negotiation.renegotiation_queued = true;
  atomic_store(&stale.refcount, SFU_RENEGOTIATION_QUEUE_CAP + 1u);
  for (uint32_t i = 0; i < 3; i++) {
    init_renegotiation_peer(&replacements[i]);
  }

  pthread_mutex_lock(&server.renegotiation_queue.lock);
  for (uint32_t i = 0; i < SFU_RENEGOTIATION_QUEUE_CAP; i++) {
    server.renegotiation_queue.items[i] = &stale;
  }
  server.renegotiation_queue.head = 0;
  server.renegotiation_queue.tail = 0;
  server.renegotiation_queue.count = SFU_RENEGOTIATION_QUEUE_CAP;
  pthread_mutex_unlock(&server.renegotiation_queue.lock);

  for (uint32_t i = 0; i < 3; i++) {
    sfu_signaling_trigger_peer_renegotiation(&replacements[i]);
    assert(replacements[i].negotiation.renegotiation_queued);
    assert(atomic_load(&replacements[i].refcount) == 2);
    assert(sfu_signaling_renegotiation_test_count(&server) == SFU_RENEGOTIATION_QUEUE_CAP);
  }
  assert(atomic_load(&stale.refcount) == SFU_RENEGOTIATION_QUEUE_CAP - 2u);
  assert(server.renegotiation_queue.fallback_count == 0);

  pthread_mutex_lock(&server.renegotiation_queue.lock);
  memset(server.renegotiation_queue.items, 0, sizeof(server.renegotiation_queue.items));
  server.renegotiation_queue.head = 0;
  server.renegotiation_queue.tail = 0;
  server.renegotiation_queue.count = 0;
  for (uint32_t i = 0; i < 3; i++) {
    pthread_mutex_lock(&replacements[i].negotiation.lock);
    replacements[i].negotiation.renegotiation_queued = false;
    pthread_mutex_unlock(&replacements[i].negotiation.lock);
  }
  pthread_mutex_unlock(&server.renegotiation_queue.lock);
  for (uint32_t i = 0; i < 3; i++) {
    release_test_queue_reference(&replacements[i]);
    replacements[i].negotiation.negotiation_needed = false;
    destroy_renegotiation_peer(&replacements[i]);
  }
  stale.negotiation.renegotiation_queued = false;
  atomic_store(&stale.refcount, 1);
  destroy_renegotiation_peer(&stale);
  sfu_signaling_renegotiation_test_server_stop(&server);
}

typedef struct renegotiation_race_ctx {
  sfu_peer_session_t *peer;
  atomic_bool running;
} renegotiation_race_ctx_t;

static void *schedule_pending_thread(void *arg) {
  renegotiation_race_ctx_t *ctx = arg;
  while (atomic_load_explicit(&ctx->running, memory_order_acquire)) {
    sfu_signaling_schedule_pending_peer(ctx->peer);
    sched_yield();
  }
  return NULL;
}

static void test_concurrent_schedule_and_pop_preserves_single_identity(void) {
  sfu_signaling_server_t server;
  sfu_signaling_renegotiation_test_server_init(&server);
  sfu_peer_session_t peer;
  init_renegotiation_peer(&peer);
  peer.negotiation.desired_offer_revision = 1;
  peer.negotiation.renegotiation_pending = true;
  peer.negotiation.negotiation_needed = true;

  renegotiation_race_ctx_t ctx = {.peer = &peer};
  atomic_store(&ctx.running, true);
  pthread_t thread;
  assert(pthread_create(&thread, NULL, schedule_pending_thread, &ctx) == 0);
  for (uint32_t i = 0; i < 10000; i++) {
    sfu_peer_session_t *queued = sfu_signaling_renegotiation_test_pop(&server);
    if (queued) {
      assert(queued == &peer);
      release_test_queue_reference(&peer);
    }
    assert(sfu_signaling_renegotiation_test_count(&server) <= 1);
    sched_yield();
  }
  atomic_store_explicit(&ctx.running, false, memory_order_release);
  pthread_join(thread, NULL);
  sfu_peer_session_t *queued;
  while ((queued = sfu_signaling_renegotiation_test_pop(&server)) != NULL) {
    assert(queued == &peer);
    release_test_queue_reference(&peer);
  }
  assert(!peer.negotiation.renegotiation_queued);
  peer.negotiation.negotiation_needed = false;
  destroy_renegotiation_peer(&peer);
  sfu_signaling_renegotiation_test_server_stop(&server);
}

int main(void) {
  test_screen_codec_preference_parsing();
  test_join_capture_failure_is_reported();
  test_queue_releases_when_signaling_is_stopped();
  test_preallocated_leave_event_released_on_stopped_queue();
  test_membership_queue_blocks_until_capacity_returns();
  test_shutdown_wakes_blocked_membership_producer();
  test_remote_slot_reconciliation_is_idempotent();
  test_remote_slot_follow_up_captures_fresh_manifest();
  test_renegotiation_queue_reclaims_closing_sessions();
  test_concurrent_schedule_and_pop_preserves_single_identity();
  printf("test_signaling_membership: OK\n");
  return 0;
}
