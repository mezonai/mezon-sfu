#include "valkey_queue.h"

#include <errno.h>
#include <string.h>

int valkey_queue_init(valkey_queue_t *queue, valkey_queue_item_fn dispose,
                      void *dispose_arg) {
  if (!queue || !dispose) {
    return EINVAL;
  }
  memset(queue, 0, sizeof(*queue));
  queue->dispose = dispose;
  queue->dispose_arg = dispose_arg;
  return pthread_mutex_init(&queue->mutex, NULL);
}

valkey_queue_post_result_t valkey_queue_post(valkey_queue_t *queue, void *item) {
  if (!queue || !item) {
    return VALKEY_QUEUE_INVALID;
  }
  pthread_mutex_lock(&queue->mutex);
  if (queue->closed) {
    pthread_mutex_unlock(&queue->mutex);
    return VALKEY_QUEUE_CLOSED;
  }
  if (queue->count == VALKEY_QUEUE_CAPACITY) {
    pthread_mutex_unlock(&queue->mutex);
    return VALKEY_QUEUE_FULL;
  }
  queue->items[queue->tail] = item;
  queue->tail = (queue->tail + 1u) % VALKEY_QUEUE_CAPACITY;
  queue->count++;
  pthread_mutex_unlock(&queue->mutex);
  return VALKEY_QUEUE_OK;
}

size_t valkey_queue_drain(valkey_queue_t *queue,
                          valkey_queue_item_fn consume, void *consume_arg) {
  if (!queue || !consume) {
    return 0;
  }
  size_t drained = 0;
  for (;;) {
    pthread_mutex_lock(&queue->mutex);
    if (queue->count == 0) {
      pthread_mutex_unlock(&queue->mutex);
      return drained;
    }
    void *item = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1u) % VALKEY_QUEUE_CAPACITY;
    queue->count--;
    pthread_mutex_unlock(&queue->mutex);
    consume(item, consume_arg);
    drained++;
  }
}

void valkey_queue_close(valkey_queue_t *queue) {
  if (!queue) {
    return;
  }
  pthread_mutex_lock(&queue->mutex);
  queue->closed = true;
  pthread_mutex_unlock(&queue->mutex);
}

void valkey_queue_destroy_after_producers_stopped(valkey_queue_t *queue) {
  if (!queue) {
    return;
  }
  valkey_queue_close(queue);
  valkey_queue_drain(queue, queue->dispose, queue->dispose_arg);
  pthread_mutex_destroy(&queue->mutex);
}
