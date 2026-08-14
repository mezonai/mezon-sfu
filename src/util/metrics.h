#ifndef SFU_UTIL_METRICS_H
#define SFU_UTIL_METRICS_H

#include <stddef.h>
#include <stdint.h>

void sfu_metrics_init(void);
void sfu_metric_inc(const char *name);
void sfu_metric_add(const char *name, uint64_t value);
uint64_t sfu_metric_get(const char *name);
size_t sfu_metrics_snapshot(char *buf, size_t cap);

#endif /* SFU_UTIL_METRICS_H */
