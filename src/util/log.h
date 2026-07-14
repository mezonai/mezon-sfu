#ifndef SFU_UTIL_LOG_H
#define SFU_UTIL_LOG_H

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
void sfu_log(sfu_log_level_t level, const char *file, int line, const char *fmt,
             ...) __attribute__((format(printf, 4, 5)));

#define SFU_LOG_DEBUG(...)                                                     \
  sfu_log(SFU_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define SFU_LOG_INFO(...)                                                      \
  sfu_log(SFU_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define SFU_LOG_WARN(...)                                                      \
  sfu_log(SFU_LOG_LEVEL_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define SFU_LOG_ERROR(...)                                                     \
  sfu_log(SFU_LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* SFU_UTIL_LOG_H */
