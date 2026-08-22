#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include "protocol/signaling/signaling.h"

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
  while (!atomic_load(&ctx.entered)) {}
  for (uint32_t i = 0; i < 10000 && !atomic_load(&ctx.completed); i++) sched_yield();
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
  while (!atomic_load(&ctx.entered)) {}
  sfu_signaling_membership_test_server_stop(&server);
  pthread_join(thread, NULL);
  assert(atomic_load(&ctx.completed));
  assert(!ctx.result);
}

int main(void) {
  test_join_capture_failure_is_reported();
  test_queue_releases_when_signaling_is_stopped();
  test_preallocated_leave_event_released_on_stopped_queue();
  test_membership_queue_blocks_until_capacity_returns();
  test_shutdown_wakes_blocked_membership_producer();
  printf("test_signaling_membership: OK\n");
  return 0;
}
