#ifndef VALKEY_QUEUE_H
#define VALKEY_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#define VALKEY_QUEUE_CAPACITY 256u

typedef enum valkey_queue_post_result {
  VALKEY_QUEUE_OK = 0,
  VALKEY_QUEUE_FULL,
  VALKEY_QUEUE_CLOSED,
  VALKEY_QUEUE_INVALID,
} valkey_queue_post_result_t;

typedef void (*valkey_queue_item_fn)(void *item, void *arg);

typedef struct valkey_queue {
  pthread_mutex_t mutex;
  size_t head;
  size_t tail;
  size_t count;
  bool closed;
  void *items[VALKEY_QUEUE_CAPACITY];
  valkey_queue_item_fn dispose;
  void *dispose_arg;
} valkey_queue_t;

int valkey_queue_init(valkey_queue_t *queue, valkey_queue_item_fn dispose,
                      void *dispose_arg);

/* OK transfers ownership. FULL/CLOSED/INVALID leave ownership with caller.
 * NULL queue or item returns INVALID. */
valkey_queue_post_result_t valkey_queue_post(valkey_queue_t *queue, void *item);

/* Transfers each popped item to non-NULL consume. Invalid arguments return 0. */
size_t valkey_queue_drain(valkey_queue_t *queue,
                          valkey_queue_item_fn consume, void *consume_arg);

/* Idempotently prevents subsequent posts. */
void valkey_queue_close(valkey_queue_t *queue);

/* Caller guarantees all producers are joined/stopped. Pending items use the
 * dispose callback supplied at init. A second destroy call is prohibited. */
void valkey_queue_destroy_after_producers_stopped(valkey_queue_t *queue);

#endif
