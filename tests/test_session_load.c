#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "peer/session.h"
#include "runtime/epoch_reclaimer.h"
#include "runtime/worker.h"
#include "transport/dtls/dtls.h"
#include "util/log.h"

#define TEST_THREAD_COUNT 8
#define TEST_SESSIONS_PER_THREAD 1250
#define TEST_TOTAL_SESSIONS (TEST_THREAD_COUNT * TEST_SESSIONS_PER_THREAD)
#define TEST_WORKER_COUNT 4

typedef struct {
  _Atomic uint64_t sessions_created;
  _Atomic uint64_t sessions_closed;
  _Atomic uint64_t lookup_hits;
  _Atomic uint64_t lookup_misses;
  _Atomic uint64_t refcount_errors;
  _Atomic uint32_t peak_session_count;
} test_metrics_t;

static test_metrics_t g_metrics = {0};

static void generate_test_addr(struct sockaddr_storage *addr, socklen_t *addr_len, uint32_t id) {
  struct sockaddr_in *sin = (struct sockaddr_in *)addr;
  memset(sin, 0, sizeof(*sin));
  sin->sin_family = AF_INET;
  sin->sin_port = htons((uint16_t)(10000 + (id % 55535)));
  sin->sin_addr.s_addr = htonl(0x0A000000u | id);
  *addr_len = sizeof(*sin);
}

typedef struct {
  sfu_session_table_t *table;
  sfu_worker_t *workers;
  sfu_peer_session_t **sessions;
  uint32_t thread_id;
  uint32_t session_start;
  uint32_t session_count;
} thread_ctx_t;

static void advance_workers(sfu_worker_t *workers) {
  for (uint32_t i = 0; i < TEST_WORKER_COUNT; i++) {
    atomic_fetch_add_explicit(&workers[i].generation, 1, memory_order_release);
  }
}

static void count_live(sfu_peer_session_t *s, void *user) {
  (void)s;
  uint32_t *live = (uint32_t *)user;
  (*live)++;
}

static void *phase1_create_sessions(void *arg) {
  thread_ctx_t *ctx = (thread_ctx_t *)arg;

  for (uint32_t i = 0; i < ctx->session_count; i++) {
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint32_t session_id = ctx->session_start + i;

    generate_test_addr(&addr, &addr_len, session_id);

    sfu_peer_session_t *s = sfu_session_table_get_or_create(ctx->table, &addr, addr_len);
    if (!s) {
      fprintf(stderr, "Thread %u: Failed to create session %u\n", ctx->thread_id, session_id);
      continue;
    }
    ctx->sessions[i] = s;
    uint64_t created = atomic_fetch_add(&g_metrics.sessions_created, 1) + 1;
    uint32_t peak = atomic_load(&g_metrics.peak_session_count);
    while ((uint32_t)created > peak) {
      if (atomic_compare_exchange_weak(&g_metrics.peak_session_count, &peak, (uint32_t)created)) {
        break;
      }
    }
    atomic_fetch_add_explicit(&ctx->workers[ctx->thread_id % TEST_WORKER_COUNT].generation, 1, memory_order_release);
  }

  return NULL;
}

static void *phase2_lookup(void *arg) {
  thread_ctx_t *ctx = (thread_ctx_t *)arg;
  uint32_t operations = ctx->session_count / 2;

  for (uint32_t i = 0; i < operations; i++) {
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint32_t session_id = ctx->session_start + (i % ctx->session_count);
    generate_test_addr(&addr, &addr_len, session_id);

    sfu_peer_session_t *s = sfu_session_table_find(ctx->table, &addr, addr_len);
    if (s) {
      atomic_fetch_add(&g_metrics.lookup_hits, 1);
      uint32_t refcount = atomic_load(&s->refcount);
      if (refcount == 0 || refcount > 100000) {
        atomic_fetch_add(&g_metrics.refcount_errors, 1);
      }
      sfu_session_release(s);
    } else {
      atomic_fetch_add(&g_metrics.lookup_misses, 1);
    }
    atomic_fetch_add_explicit(&ctx->workers[ctx->thread_id % TEST_WORKER_COUNT].generation, 1, memory_order_release);
    if (i % 100 == 0) {
      sched_yield();
    }
  }

  return NULL;
}

static void *phase3_close_sessions(void *arg) {
  thread_ctx_t *ctx = (thread_ctx_t *)arg;

  for (uint32_t i = 0; i < ctx->session_count; i++) {
    sfu_peer_session_t *s = ctx->sessions[i];
    if (!s) {
      continue;
    }
    /* Drop the table pin, then the caller pin from get_or_create (refcount 2). */
    (void)sfu_session_begin_close(ctx->table, s);
    sfu_session_release(s);
    ctx->sessions[i] = NULL;
    atomic_fetch_add(&g_metrics.sessions_closed, 1);
    advance_workers(ctx->workers);
    if ((i % 32u) == 0u && ctx->table->reclaimer) {
      (void)sfu_epoch_reclaimer_sweep(ctx->table->reclaimer);
    }
  }

  return NULL;
}

static void measure_hash_performance(sfu_session_table_t *table) {
  uint32_t used_slots = 0;

  for (uint32_t i = 0; i < SFU_SESSION_ADDR_HASH_SLOTS; i++) {
    if (table->addr_index[i].index != SFU_HASH_EMPTY && table->addr_index[i].index != SFU_HASH_DELETED) {
      used_slots++;
    }
  }

  float load_factor = (float)used_slots / (float)SFU_SESSION_ADDR_HASH_SLOTS;
  printf("Hash table performance:\n");
  printf("  Used slots: %u / %u\n", used_slots, SFU_SESSION_ADDR_HASH_SLOTS);
  printf("  Load factor: %.4f\n", load_factor);
  printf("  Avg probe depth estimate: %.2f\n", load_factor < 1.0f ? 1.0f / (1.0f - load_factor) : 999.0f);
}

int main(void) {
  sfu_log_set_level(SFU_LOG_LEVEL_ERROR);

  printf("\n=== Session Load Test: 10k+ Concurrent Operations ===\n\n");

  /* Real worker layout so get_worker_generation() indexes generation correctly. */
  sfu_worker_t *workers = calloc(TEST_WORKER_COUNT, sizeof(*workers));
  assert(workers != NULL);
  for (uint32_t i = 0; i < TEST_WORKER_COUNT; i++) {
    atomic_store_explicit(&workers[i].generation, 0, memory_order_relaxed);
  }

  sfu_dtls_ctx_t dtls_ctx;
  assert(sfu_dtls_ctx_init(&dtls_ctx) == 0);

  sfu_session_table_t table;
  if (sfu_session_table_init(&table, &dtls_ctx, workers, TEST_WORKER_COUNT) != 0) {
    fprintf(stderr, "Failed to initialize session table\n");
    sfu_dtls_ctx_destroy(&dtls_ctx);
    free(workers);
    return 1;
  }

  printf("Initialized session table with epoch reclaimer (capacity %d)\n", SFU_EPOCH_RECLAIMER_CAPACITY);
  printf("Configuration: %d threads x %d sessions = %d total\n\n", TEST_THREAD_COUNT, TEST_SESSIONS_PER_THREAD, TEST_TOTAL_SESSIONS);

  thread_ctx_t contexts[TEST_THREAD_COUNT];
  pthread_t threads[TEST_THREAD_COUNT];

  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    contexts[i].table = &table;
    contexts[i].workers = workers;
    contexts[i].thread_id = i;
    contexts[i].session_start = i * TEST_SESSIONS_PER_THREAD;
    contexts[i].session_count = TEST_SESSIONS_PER_THREAD;
    contexts[i].sessions = calloc(TEST_SESSIONS_PER_THREAD, sizeof(sfu_peer_session_t *));
    assert(contexts[i].sessions != NULL);
  }

  printf("Phase 1: Ramp-up - Creating %d sessions concurrently...\n", TEST_TOTAL_SESSIONS);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    assert(pthread_create(&threads[i], NULL, phase1_create_sessions, &contexts[i]) == 0);
  }
  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    pthread_join(threads[i], NULL);
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  double phase1_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

  printf("  Created: %lu sessions in %.3f seconds (%.0f sessions/sec)\n", atomic_load(&g_metrics.sessions_created), phase1_time,
         phase1_time > 0 ? atomic_load(&g_metrics.sessions_created) / phase1_time : 0);
  printf("  Peak session count: %u\n", atomic_load(&g_metrics.peak_session_count));
  measure_hash_performance(&table);
  printf("\n");

  printf("Phase 2: Lookup - find + release of find-pin...\n");
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    assert(pthread_create(&threads[i], NULL, phase2_lookup, &contexts[i]) == 0);
  }
  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    pthread_join(threads[i], NULL);
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  double phase2_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
  uint64_t total_lookups = atomic_load(&g_metrics.lookup_hits) + atomic_load(&g_metrics.lookup_misses);
  float hit_rate = total_lookups > 0 ? (float)atomic_load(&g_metrics.lookup_hits) / (float)total_lookups * 100.0f : 0.0f;
  printf("  Lookups: %lu total (%.1f%% hit rate) in %.3f seconds\n", total_lookups, hit_rate, phase2_time);
  printf("  Refcount errors: %lu\n\n", atomic_load(&g_metrics.refcount_errors));

  printf("Phase 3: Close - begin_close + caller-pin release...\n");
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    assert(pthread_create(&threads[i], NULL, phase3_close_sessions, &contexts[i]) == 0);
  }
  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    pthread_join(threads[i], NULL);
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  double phase3_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
  printf("  Closed: %lu sessions in %.3f seconds\n\n", atomic_load(&g_metrics.sessions_closed), phase3_time);

  printf("Phase 4: Sweep until epoch reclaimer drains...\n");
  uint32_t total_reclaimed = 0;
  for (int sweep = 0; sweep < 64; sweep++) {
    advance_workers(workers);
    uint32_t reclaimed = table.reclaimer ? sfu_epoch_reclaimer_sweep(table.reclaimer) : 0;
    total_reclaimed += reclaimed;
    if (reclaimed == 0 && sweep > 2) {
      break;
    }
  }
  printf("  Sweep-after-close reclaimed: %u (more may have been reclaimed during close)\n", total_reclaimed);

  uint32_t live = 0;
  sfu_session_table_foreach(&table, count_live, &live);
  printf("  Live sessions after close: %u\n\n", live);

  for (uint32_t i = 0; i < TEST_THREAD_COUNT; i++) {
    free(contexts[i].sessions);
  }

  sfu_session_table_destroy(&table);
  sfu_dtls_ctx_destroy(&dtls_ctx);
  free(workers);

  printf("=== Test Results ===\n");
  printf("Sessions created: %lu\n", atomic_load(&g_metrics.sessions_created));
  printf("Sessions closed: %lu\n", atomic_load(&g_metrics.sessions_closed));
  printf("Peak concurrent sessions: %u\n", atomic_load(&g_metrics.peak_session_count));
  printf("Refcount errors: %lu\n", atomic_load(&g_metrics.refcount_errors));
  printf("Live after close: %u\n", live);

  bool passed = true;
  if (atomic_load(&g_metrics.sessions_created) != TEST_TOTAL_SESSIONS) {
    fprintf(stderr, "FAIL: Expected %d sessions created, got %lu\n", TEST_TOTAL_SESSIONS, atomic_load(&g_metrics.sessions_created));
    passed = false;
  }
  if (atomic_load(&g_metrics.sessions_closed) != TEST_TOTAL_SESSIONS) {
    fprintf(stderr, "FAIL: Expected %d sessions closed, got %lu\n", TEST_TOTAL_SESSIONS, atomic_load(&g_metrics.sessions_closed));
    passed = false;
  }
  if (atomic_load(&g_metrics.refcount_errors) > 0) {
    fprintf(stderr, "FAIL: Detected %lu refcount errors\n", atomic_load(&g_metrics.refcount_errors));
    passed = false;
  }
  if (live != 0) {
    fprintf(stderr, "FAIL: Expected 0 live sessions after close, got %u\n", live);
    passed = false;
  }
  if (atomic_load(&g_metrics.lookup_misses) > 0) {
    fprintf(stderr, "FAIL: Expected 0 lookup misses while sessions were live, got %lu\n", atomic_load(&g_metrics.lookup_misses));
    passed = false;
  }

  printf("\n");
  if (passed) {
    printf("All validations PASSED\n");
    printf("test_session_load: OK\n");
    return 0;
  }
  printf("Some validations FAILED\n");
  return 1;
}
