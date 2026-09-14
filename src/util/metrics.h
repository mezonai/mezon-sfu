#ifndef SFU_UTIL_METRICS_H
#define SFU_UTIL_METRICS_H

#include <stddef.h>
#include <stdint.h>

#include "util/metric_list.h"

typedef enum sfu_metric_id {
#define SFU_METRIC_ENUM(id, name) SFU_METRIC_##id,
  SFU_METRIC_LIST(SFU_METRIC_ENUM)
#undef SFU_METRIC_ENUM
  SFU_METRIC_COUNT
} sfu_metric_id_t;

void sfu_metrics_init(void);

/* Fast path for call sites whose metric is known at compile time. */
void sfu_metric_inc_id(sfu_metric_id_t id);
void sfu_metric_add_id(sfu_metric_id_t id, uint64_t value);
uint64_t sfu_metric_get_id(sfu_metric_id_t id);

/* Name-based compatibility API for dynamic/external callers. */
void sfu_metric_inc(const char *name);
void sfu_metric_add(const char *name, uint64_t value);
uint64_t sfu_metric_get(const char *name);
size_t sfu_metrics_snapshot(char *buf, size_t cap);

#endif /* SFU_UTIL_METRICS_H */
