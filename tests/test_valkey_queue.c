#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "cache/valkey_queue.h"

typedef struct test_stats {
  atomic_size_t consumed;
  atomic_size_t disposed;
} test_stats_t;

static void consume_item(void *item, void *arg) {
  test_stats_t *stats = arg;
  free(item);
  atomic_fetch_add(&stats->consumed, 1);
}

static void dispose_pending(void *item, void *arg) {
  test_stats_t *stats = arg;
  free(item);
  atomic_fetch_add(&stats->disposed, 1);
}

static void *new_item(void) {
  void *item = malloc(1);
  assert(item);
  return item;
}

static void test_invalid_arguments(void) {
  test_stats_t stats = {0};
  valkey_queue_t queue;
  assert(valkey_queue_init(NULL, dispose_pending, &stats) != 0);
  assert(valkey_queue_init(&queue, NULL, &stats) != 0);
  assert(valkey_queue_init(&queue, dispose_pending, &stats) == 0);
  void *orphan = new_item();
  assert(valkey_queue_post(NULL, orphan) == VALKEY_QUEUE_INVALID);
  free(orphan);
  assert(valkey_queue_post(&queue, NULL) == VALKEY_QUEUE_INVALID);
  assert(valkey_queue_drain(NULL, consume_item, &stats) == 0);
  assert(valkey_queue_drain(&queue, NULL, &stats) == 0);
  valkey_queue_close(NULL);
  valkey_queue_destroy_after_producers_stopped(&queue);
}

static void test_capacity_and_drain(void) {
  test_stats_t stats = {0};
  valkey_queue_t queue;
  assert(valkey_queue_init(&queue, dispose_pending, &stats) == 0);
  for (size_t i = 0; i < VALKEY_QUEUE_CAPACITY; i++) {
    assert(valkey_queue_post(&queue, new_item()) == VALKEY_QUEUE_OK);
  }
  void *extra = new_item();
  assert(valkey_queue_post(&queue, extra) == VALKEY_QUEUE_FULL);
  free(extra);
  assert(valkey_queue_drain(&queue, consume_item, &stats) == VALKEY_QUEUE_CAPACITY);
  assert(atomic_load(&stats.consumed) == VALKEY_QUEUE_CAPACITY);
  assert(valkey_queue_drain(&queue, consume_item, &stats) == 0);
  valkey_queue_destroy_after_producers_stopped(&queue);
}

typedef struct producer {
  valkey_queue_t *queue;
  size_t accepted;
} producer_t;

static void *produce(void *arg) {
  producer_t *producer = arg;
  for (size_t i = 0; i < 64; i++) {
    void *item = new_item();
    valkey_queue_post_result_t result = valkey_queue_post(producer->queue, item);
    if (result == VALKEY_QUEUE_OK) {
      producer->accepted++;
    } else {
      assert(result == VALKEY_QUEUE_FULL);
      free(item);
    }
  }
  return NULL;
}

static void test_concurrent_producers(void) {
  test_stats_t stats = {0};
  valkey_queue_t queue;
  pthread_t threads[8];
  producer_t producers[8] = {0};
  size_t accepted = 0;
  assert(valkey_queue_init(&queue, dispose_pending, &stats) == 0);
  for (size_t i = 0; i < 8; i++) {
    producers[i].queue = &queue;
    assert(pthread_create(&threads[i], NULL, produce, &producers[i]) == 0);
  }
  for (size_t i = 0; i < 8; i++) {
    pthread_join(threads[i], NULL);
    accepted += producers[i].accepted;
  }
  assert(accepted == VALKEY_QUEUE_CAPACITY);
  assert(valkey_queue_drain(&queue, consume_item, &stats) == accepted);
  assert(atomic_load(&stats.consumed) == accepted);
  valkey_queue_destroy_after_producers_stopped(&queue);
}

static void test_close_and_pending(void) {
  test_stats_t stats = {0};
  valkey_queue_t queue;
  assert(valkey_queue_init(&queue, dispose_pending, &stats) == 0);
  for (size_t i = 0; i < 17; i++) {
    assert(valkey_queue_post(&queue, new_item()) == VALKEY_QUEUE_OK);
  }
  valkey_queue_close(&queue);
  valkey_queue_close(&queue);
  void *extra = new_item();
  assert(valkey_queue_post(&queue, extra) == VALKEY_QUEUE_CLOSED);
  free(extra);
  valkey_queue_destroy_after_producers_stopped(&queue);
  assert(atomic_load(&stats.disposed) == 17);
}

static void test_public_wraparound(void) {
  test_stats_t stats = {0};
  valkey_queue_t queue;
  assert(valkey_queue_init(&queue, dispose_pending, &stats) == 0);
  for (size_t round = 0; round < 3; round++) {
    for (size_t i = 0; i < VALKEY_QUEUE_CAPACITY; i++) {
      assert(valkey_queue_post(&queue, new_item()) == VALKEY_QUEUE_OK);
    }
    assert(valkey_queue_drain(&queue, consume_item, &stats) == VALKEY_QUEUE_CAPACITY);
  }
  assert(atomic_load(&stats.consumed) == 3 * VALKEY_QUEUE_CAPACITY);
  valkey_queue_destroy_after_producers_stopped(&queue);
}

/* #86: producers racing valkey_queue_close — the shutdown interleaving.
 * Producers must see exactly one of OK (item queued, drained later),
 * FULL (saturation backpressure), or CLOSED; every accepted item must be
 * accounted exactly once between drain and dispose, and every non-OK item
 * stays with the producer (freed here). Under TSan this witnesses the
 * close-vs-post mutex contract. */
typedef struct {
  valkey_queue_t *queue;
  size_t accepted;
  size_t rejected; /* FULL or CLOSED: ownership stayed with producer */
} race_producer_t;

static void *produce_until_closed(void *arg) {
  race_producer_t *p = arg;
  for (size_t i = 0; i < 2000; i++) {
    void *item = new_item();
    valkey_queue_post_result_t rc = valkey_queue_post(p->queue, item);
    if (rc == VALKEY_QUEUE_OK) {
      p->accepted++;
    } else {
      assert(rc == VALKEY_QUEUE_FULL || rc == VALKEY_QUEUE_CLOSED);
      free(item);
      p->rejected++;
    }
  }
  return NULL;
}

static void test_producer_close_race(void) {
  test_stats_t stats = {0};
  valkey_queue_t queue;
  assert(valkey_queue_init(&queue, dispose_pending, &stats) == 0);

  race_producer_t producers[4] = {0};
  pthread_t threads[4];
  for (size_t i = 0; i < 4; i++) {
    producers[i].queue = &queue;
    assert(pthread_create(&threads[i], NULL, produce_until_closed, &producers[i]) == 0);
  }
  /* Close concurrently with the producer storm. */
  valkey_queue_close(&queue);
  for (size_t i = 0; i < 4; i++) {
    pthread_join(threads[i], NULL);
  }

  size_t accepted = 0;
  for (size_t i = 0; i < 4; i++) {
    accepted += producers[i].accepted;
    assert(producers[i].accepted + producers[i].rejected == 2000);
  }
  assert(accepted <= VALKEY_QUEUE_CAPACITY);

  /* Every accepted item is accounted exactly once: drained now, or
   * disposed at destroy. No leak, no double-free. */
  size_t drained = valkey_queue_drain(&queue, consume_item, &stats);
  valkey_queue_destroy_after_producers_stopped(&queue);
  assert(atomic_load(&stats.consumed) == drained);
  assert(drained + atomic_load(&stats.disposed) == accepted);
}

int main(void) {
  test_invalid_arguments();
  test_capacity_and_drain();
  test_concurrent_producers();
  test_close_and_pending();
  test_public_wraparound();
  test_producer_close_race();
  return 0;
}
