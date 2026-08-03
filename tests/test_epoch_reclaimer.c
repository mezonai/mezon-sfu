#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "runtime/epoch_reclaimer.h"

typedef struct { _Atomic uint64_t generations[2]; } epochs_t;
static _Atomic uint32_t destroyed;

static uint64_t generation(void *ctx, uint32_t i) {
  epochs_t *epochs = ctx;
  return atomic_load(&epochs->generations[i]);
}
static void destroy(void *ptr) { atomic_fetch_add(&destroyed, 1); free(ptr); }

typedef struct {
  sfu_epoch_reclaimer_t *r;
  epochs_t *epochs;
} concurrent_ctx_t;
static void *retire_loop(void *arg) {
  concurrent_ctx_t *ctx = arg;
  for (uint32_t i = 0; i < 50000; i++) {
    void *ptr = malloc(1);
    while (!sfu_epoch_reclaimer_retire(ctx->r, ptr, destroy)) {
      sfu_epoch_reclaimer_sweep(ctx->r);
    }
    atomic_fetch_add(&ctx->epochs->generations[0], 1);
    atomic_fetch_add(&ctx->epochs->generations[1], 1);
  }
  return NULL;
}
static void *sweep_loop(void *arg) {
  concurrent_ctx_t *ctx = arg;
  for (uint32_t i = 0; i < 50000; i++) {
    sfu_epoch_reclaimer_sweep(ctx->r);
  }
  return NULL;
}

int main(void) {
  epochs_t epochs = {0};
  sfu_epoch_reclaimer_t r;
  assert(sfu_epoch_reclaimer_init(&r, 2, generation, &epochs) == 0);

  assert(sfu_epoch_reclaimer_retire(&r, malloc(1), destroy));
  atomic_fetch_add(&epochs.generations[0], 1);
  assert(sfu_epoch_reclaimer_sweep(&r) == 0); /* worker 1 is stalled */
  assert(atomic_load(&destroyed) == 0);
  atomic_fetch_add(&epochs.generations[1], 1);
  assert(sfu_epoch_reclaimer_sweep(&r) == 1);
  assert(atomic_load(&destroyed) == 1); /* exactly once */
  assert(sfu_epoch_reclaimer_sweep(&r) == 0);

  void *retained = NULL;
  for (uint32_t i = 0; i < SFU_EPOCH_RECLAIMER_CAPACITY; i++)
    assert(sfu_epoch_reclaimer_retire(&r, malloc(1), destroy));
  retained = malloc(1);
  assert(!sfu_epoch_reclaimer_retire(&r, retained, destroy));
  assert(atomic_load(&destroyed) == 1); /* exhaustion never destroys */
  free(retained); /* caller retained ownership */
  sfu_epoch_reclaimer_destroy_after_quiescence(&r);
  assert(atomic_load(&destroyed) == 1 + SFU_EPOCH_RECLAIMER_CAPACITY);

  /* Concurrent retire/sweep smoke: no leaks or double-destruction under
   * quiescent final drain. */
  atomic_store(&destroyed, 0);
  assert(sfu_epoch_reclaimer_init(&r, 2, generation, &epochs) == 0);
  concurrent_ctx_t ctx = {.r = &r, .epochs = &epochs};
  pthread_t retire_thread, sweep_thread;
  assert(pthread_create(&retire_thread, NULL, retire_loop, &ctx) == 0);
  assert(pthread_create(&sweep_thread, NULL, sweep_loop, &ctx) == 0);
  assert(pthread_join(retire_thread, NULL) == 0);
  assert(pthread_join(sweep_thread, NULL) == 0);
  sfu_epoch_reclaimer_destroy_after_quiescence(&r);
  assert(atomic_load(&destroyed) == 50000); /* every retired ptr freed once */

  printf("test_epoch_reclaimer: OK\n");
  return 0;
}
