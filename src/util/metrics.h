#ifndef SFU_UTIL_METRICS_H
#define SFU_UTIL_METRICS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal dependency-free metrics registry.
 *
 * Fixed table of named counters, zeroed at sfu_metrics_init(). Counters are
 * registered at compile time (see metrics.c); later PRs instrument call sites
 * by name. No dynamic registration, no external deps.
 *
 * Thread-safety: increments use memory_order_relaxed atomics. Snapshot is
 * best-effort — values for different counters may tear relative to each
 * other; that is acceptable for ops dashboards and tests.
 */

void sfu_metrics_init(void);

/* Increment named counter by 1. No-op if name is NULL or unknown. */
void sfu_metric_inc(const char *name);

/* Read current value. Returns 0 if name is NULL or unknown. */
uint64_t sfu_metric_get(const char *name);

/*
 * Render "name value" lines (one per counter, trailing newline each) into
 * buf. Always NUL-terminates when cap > 0. Returns the number of bytes that
 * would have been written excluding the NUL (snprintf-style); if the return
 * value is >= cap the output was truncated.
 */
size_t sfu_metrics_snapshot(char *buf, size_t cap);

#endif /* SFU_UTIL_METRICS_H */
