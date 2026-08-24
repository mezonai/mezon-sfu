#include "util/metrics.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define INCS_PER_THREAD 10000
#define COUNTER_NAME "msg_trunc_drop"

#define EXPECT(cond)                                                     \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      exit(1);                                                           \
    }                                                                    \
  } while (0)

static void *inc_worker(void *arg) {
  (void)arg;
  for (int i = 0; i < INCS_PER_THREAD; i++) {
    sfu_metric_inc(COUNTER_NAME);
  }
  return NULL;
}

static void test_init_and_get(void) {
  sfu_metrics_init();
  EXPECT(sfu_metric_get("msg_trunc_drop") == 0);
  EXPECT(sfu_metric_get("json_reject") == 0);
  EXPECT(sfu_metric_get("unknown_metric") == 0);
  EXPECT(sfu_metric_get(NULL) == 0);
}

static void test_inc_get(void) {
  sfu_metrics_init();

  sfu_metric_inc("msg_trunc_drop");
  sfu_metric_inc("msg_trunc_drop");
  sfu_metric_inc("json_reject");
  sfu_metric_inc("not_a_real_counter"); /* no-op */
  sfu_metric_inc(NULL);                  /* no-op */

  EXPECT(sfu_metric_get("msg_trunc_drop") == 2);
  EXPECT(sfu_metric_get("json_reject") == 1);
  EXPECT(sfu_metric_get("not_a_real_counter") == 0);
}

static void test_snapshot_format(void) {
  sfu_metrics_init();
  sfu_metric_inc("json_reject");
  sfu_metric_inc("json_reject");
  sfu_metric_inc("json_reject");
  sfu_metric_add("egress_copied_bytes", 1234);

  char buf[4096]; /* table grows as counters are added */
  size_t n = sfu_metrics_snapshot(buf, sizeof(buf));
  EXPECT(n > 0);
  EXPECT(n < sizeof(buf)); /* fits */
  EXPECT(strlen(buf) == n);

  /* Expect one "name value\n" line per registered counter, in table order. */
  EXPECT(strstr(buf, "msg_trunc_drop 0\n") != NULL);
  EXPECT(strstr(buf, "json_reject 3\n") != NULL);
  EXPECT(strstr(buf, "egress_copied_bytes 1234\n") != NULL);

  /* Every line is "name value" with a single space and trailing newline. */
  const char *p = buf;
  int lines = 0;
  while (*p) {
    const char *nl = strchr(p, '\n');
    EXPECT(nl != NULL);
    const char *sp = strchr(p, ' ');
    EXPECT(sp != NULL && sp < nl);
    /* no second space before newline */
    EXPECT(memchr(sp + 1, ' ', (size_t)(nl - sp - 1)) == NULL);
    p = nl + 1;
    lines++;
  }
  EXPECT(lines >= 2); /* registry grows as new counters are registered */

  /* New counters render too. */
  EXPECT(strstr(buf, "rtcp_compound_malformed 0\n") != NULL);
  EXPECT(strstr(buf, "rtx_build_fail 0\n") != NULL);
  EXPECT(strstr(buf, "rtx_seq_translate_fail 0\n") != NULL);
  EXPECT(strstr(buf, "rtx_protect_replay_old 0\n") != NULL);
  EXPECT(strstr(buf, "dtls_restart_detected 0\n") != NULL);
  EXPECT(strstr(buf, "ingress_unprotect_auth_fail 0\n") != NULL);
  EXPECT(strstr(buf, "ingress_unprotect_previous_generation 0\n") != NULL);
  EXPECT(strstr(buf, "bandwidth_allocator_runs 0\n") != NULL);
  EXPECT(strstr(buf, "bandwidth_allocator_active_streams 0\n") != NULL);
  EXPECT(strstr(buf, "bandwidth_allocator_unallocated_bps 0\n") != NULL);
  EXPECT(strstr(buf, "remb_contribution_stale 0\n") != NULL);
  EXPECT(strstr(buf, "remb_aggregate_no_fresh 0\n") != NULL);
  EXPECT(strstr(buf, "remb_aggregate_target_changed 0\n") != NULL);
  EXPECT(strstr(buf, "congestion_pli_received 0\n") != NULL);

  /* Truncation: tiny buffer still NUL-terminated; return is full length. */
  char tiny[8];
  size_t full = sfu_metrics_snapshot(tiny, sizeof(tiny));
  EXPECT(full == n);
  EXPECT(strlen(tiny) < sizeof(tiny));
  EXPECT(tiny[0] != '\0');
}

static void test_multithreaded_inc(void) {
  sfu_metrics_init();

  pthread_t threads[THREADS];
  for (int i = 0; i < THREADS; i++) {
    EXPECT(pthread_create(&threads[i], NULL, inc_worker, NULL) == 0);
  }
  for (int i = 0; i < THREADS; i++) {
    EXPECT(pthread_join(threads[i], NULL) == 0);
  }

  uint64_t total = sfu_metric_get(COUNTER_NAME);
  EXPECT(total == (uint64_t)THREADS * INCS_PER_THREAD);
  EXPECT(sfu_metric_get("json_reject") == 0);
}

int main(void) {
  test_init_and_get();
  test_inc_get();
  test_snapshot_format();
  test_multithreaded_inc();

  printf("test_metrics: OK\n");
  return 0;
}
