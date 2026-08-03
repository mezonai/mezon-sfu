#include "runtime/epoch_reclaimer.h"

#include <string.h>

#include "util/log.h"

int sfu_epoch_reclaimer_init(sfu_epoch_reclaimer_t *reclaimer, uint32_t worker_count,
                             sfu_epoch_generation_fn generation, void *generation_context) {
  if (!reclaimer || worker_count == 0 || worker_count > SFU_MAX_WORKERS || !generation) {
    SFU_LOG_ERROR("epoch_reclaimer: invalid init args");
    return -1;
  }

  memset(reclaimer, 0, sizeof(*reclaimer));
  if (pthread_mutex_init(&reclaimer->lock, NULL) != 0) {
    SFU_LOG_ERROR("epoch_reclaimer: failed to init lock");
    return -1;
  }

  reclaimer->generation = generation;
  reclaimer->generation_context = generation_context;
  reclaimer->worker_count = worker_count;

  for (uint32_t i = 0; i < SFU_EPOCH_RECLAIMER_CAPACITY; i++) {
    reclaimer->nodes[i].next = reclaimer->free_nodes;
    reclaimer->free_nodes = &reclaimer->nodes[i];
  }

  return 0;
}

bool sfu_epoch_reclaimer_retire(sfu_epoch_reclaimer_t *reclaimer, void *ptr,
                                sfu_epoch_destructor_fn destructor) {
  if (!reclaimer || !ptr || !destructor) {
    return false;
  }

  pthread_mutex_lock(&reclaimer->lock);
  if (!reclaimer->free_nodes) {
    pthread_mutex_unlock(&reclaimer->lock);
    return false;
  }

  sfu_epoch_retire_node_t *node = reclaimer->free_nodes;
  reclaimer->free_nodes = node->next;
  node->ptr = ptr;
  node->destructor = destructor;
  for (uint32_t i = 0; i < reclaimer->worker_count; i++) {
    node->worker_generations[i] = reclaimer->generation(reclaimer->generation_context, i);
  }
  node->next = reclaimer->pending;
  reclaimer->pending = node;
  pthread_mutex_unlock(&reclaimer->lock);

  return true;
}

uint32_t sfu_epoch_reclaimer_sweep(sfu_epoch_reclaimer_t *reclaimer) {
  if (!reclaimer) {
    return 0;
  }

  uint32_t reclaimed = 0;
  sfu_epoch_retire_node_t *ready = NULL;

  pthread_mutex_lock(&reclaimer->lock);
  sfu_epoch_retire_node_t **cursor = &reclaimer->pending;
  while (*cursor) {
    sfu_epoch_retire_node_t *node = *cursor;
    bool safe = true;
    for (uint32_t i = 0; i < reclaimer->worker_count; i++) {
      if (reclaimer->generation(reclaimer->generation_context, i) == node->worker_generations[i]) {
        safe = false;
        break;
      }
    }

    if (safe) {
      *cursor = node->next;
      node->next = ready;
      ready = node;
      reclaimed++;
    } else {
      cursor = &node->next;
    }
  }
  pthread_mutex_unlock(&reclaimer->lock);

  while (ready) {
    sfu_epoch_retire_node_t *node = ready;
    ready = node->next;
    node->destructor(node->ptr);
    pthread_mutex_lock(&reclaimer->lock);
    node->next = reclaimer->free_nodes;
    reclaimer->free_nodes = node;
    pthread_mutex_unlock(&reclaimer->lock);
  }

  return reclaimed;
}

void sfu_epoch_reclaimer_destroy_after_quiescence(sfu_epoch_reclaimer_t *reclaimer) {
  if (!reclaimer) {
    return;
  }

  pthread_mutex_lock(&reclaimer->lock);
  sfu_epoch_retire_node_t *node = reclaimer->pending;
  reclaimer->pending = NULL;
  pthread_mutex_unlock(&reclaimer->lock);

  while (node) {
    sfu_epoch_retire_node_t *next = node->next;
    node->destructor(node->ptr);
    node = next;
  }

  pthread_mutex_destroy(&reclaimer->lock);
}
