#include "util/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static _Atomic int g_min_level = SFU_LOG_LEVEL_INFO;

#define SFU_LOG_RATE_BUCKET_COUNT 64u

static _Atomic int64_t g_rate_last_ns[SFU_LOG_RATE_BUCKET_COUNT];

static uint64_t rate_monotonic_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t rate_bucket_hash(const char *bucket) {
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)bucket; *p; p++) {
    h ^= (uint32_t)*p;
    h *= 16777619u;
  }
  return h;
}

bool sfu_log_rate_limit(const char *bucket, uint64_t interval_ns) {
  if (!bucket || interval_ns == 0) {
    return true;
  }
  uint32_t h = rate_bucket_hash(bucket);
  uint64_t now = rate_monotonic_ns();
  int64_t now_signed = (int64_t)now;

  for (uint32_t i = 0; i < SFU_LOG_RATE_BUCKET_COUNT; i++) {
    uint32_t idx = (h + i) & (SFU_LOG_RATE_BUCKET_COUNT - 1u);
    int64_t last = atomic_load_explicit(&g_rate_last_ns[idx], memory_order_relaxed);
    if (last != 0 && now_signed - last >= 0 && now_signed - last < (int64_t)interval_ns) {
      return false;
    }
    if (atomic_compare_exchange_strong_explicit(&g_rate_last_ns[idx], &last, now_signed, memory_order_relaxed, memory_order_relaxed)) {
      return true;
    }
    last = atomic_load_explicit(&g_rate_last_ns[idx], memory_order_relaxed);
    if (last != 0 && now_signed - last >= 0 && now_signed - last < (int64_t)interval_ns) {
      return false;
    }
  }
  return true;
}

void sfu_log_rate_limit_reset(void) {
  for (uint32_t i = 0; i < SFU_LOG_RATE_BUCKET_COUNT; i++) {
    atomic_store_explicit(&g_rate_last_ns[i], 0, memory_order_relaxed);
  }
}

static const char *level_name(sfu_log_level_t level) {
  switch (level) {
    case SFU_LOG_LEVEL_DEBUG:
      return "DEBUG";
    case SFU_LOG_LEVEL_INFO:
      return "INFO ";
    case SFU_LOG_LEVEL_WARN:
      return "WARN ";
    case SFU_LOG_LEVEL_ERROR:
      return "ERROR";
  }
  return "INVALID";
}

void sfu_log_set_level(sfu_log_level_t level) { atomic_store_explicit(&g_min_level, level, memory_order_relaxed); }

void sfu_log(sfu_log_level_t level, const char *file, int line, const char *fmt, ...) {
  if ((int)level < atomic_load_explicit(&g_min_level, memory_order_relaxed)) {
    return;
  }

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  fprintf(stderr, "[%ld.%03ld] %s %s:%d: ", (long)ts.tv_sec, ts.tv_nsec / 1000000L, level_name(level), file, line);

  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  fputc('\n', stderr);
}
