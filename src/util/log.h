#ifndef SFU_UTIL_LOG_H
#define SFU_UTIL_LOG_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Minimal leveled logger. Deliberately not fancy: this is a hot-path
 * media server, so logging on the packet path should be rare (state
 * transitions, errors) and never per-packet in steady state.
 */

typedef enum {
  SFU_LOG_LEVEL_DEBUG = 0,
  SFU_LOG_LEVEL_INFO,
  SFU_LOG_LEVEL_WARN,
  SFU_LOG_LEVEL_ERROR,
} sfu_log_level_t;

void sfu_log_set_level(sfu_log_level_t level);
void sfu_log(sfu_log_level_t level, const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/*
 * Rate limiter for logs emitted from run-loops (backend backpressure,
 * egress congestion notices). Returns true for at most one call per
 * `interval_ns` per bucket tag, so a busy loop can keep a diagnostic line
 * without flooding stderr. The bucket is expected to be a string literal
 * with static storage duration; it is only used as a key in the hashed
 * bucket table.
 */
bool sfu_log_rate_limit(const char *bucket, uint64_t interval_ns);
void sfu_log_rate_limit_reset(void);

#define SFU_LOG_DEBUG(...) sfu_log(SFU_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define SFU_LOG_INFO(...) sfu_log(SFU_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define SFU_LOG_WARN(...) sfu_log(SFU_LOG_LEVEL_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define SFU_LOG_ERROR(...) sfu_log(SFU_LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* SFU_UTIL_LOG_H */
