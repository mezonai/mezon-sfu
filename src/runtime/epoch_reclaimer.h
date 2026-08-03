#ifndef SFU_RUNTIME_EPOCH_RECLAIMER_H
#define SFU_RUNTIME_EPOCH_RECLAIMER_H

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include "sfu/config.h"

#define SFU_EPOCH_RECLAIMER_CAPACITY 256

typedef uint64_t (*sfu_epoch_generation_fn)(void *context, uint32_t worker_index);
typedef void (*sfu_epoch_destructor_fn)(void *ptr);

typedef struct sfu_epoch_retire_node {
  void *ptr;
  sfu_epoch_destructor_fn destructor;
  uint64_t worker_generations[SFU_MAX_WORKERS];
  struct sfu_epoch_retire_node *next;
} sfu_epoch_retire_node_t;

typedef struct sfu_epoch_reclaimer {
  pthread_mutex_t lock;
  sfu_epoch_retire_node_t *pending;
  sfu_epoch_retire_node_t *free_nodes;
  sfu_epoch_generation_fn generation;
  void *generation_context;
  uint32_t worker_count;
  sfu_epoch_retire_node_t nodes[SFU_EPOCH_RECLAIMER_CAPACITY];
} sfu_epoch_reclaimer_t;

int sfu_epoch_reclaimer_init(sfu_epoch_reclaimer_t *reclaimer, uint32_t worker_count,
                             sfu_epoch_generation_fn generation, void *generation_context);
/* Retires ptr using a snapshot of every worker generation. Returns false on
 * pool exhaustion; ownership remains with the caller and ptr is never freed. */
bool sfu_epoch_reclaimer_retire(sfu_epoch_reclaimer_t *reclaimer, void *ptr,
                                sfu_epoch_destructor_fn destructor);
/* Reclaims records for which every snapshotted worker generation advanced. */
uint32_t sfu_epoch_reclaimer_sweep(sfu_epoch_reclaimer_t *reclaimer);
/* Workers must be stopped/quiescent. Drains all records regardless of epochs. */
void sfu_epoch_reclaimer_destroy_after_quiescence(sfu_epoch_reclaimer_t *reclaimer);

#endif
