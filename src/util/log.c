#include "util/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

static _Atomic int g_min_level = SFU_LOG_LEVEL_INFO;

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
