#include "util/metrics.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SFU_METRIC_NAME(id, name) name,
static const char *const k_metric_names[SFU_METRIC_COUNT] = {SFU_METRIC_LIST(SFU_METRIC_NAME)};
#undef SFU_METRIC_NAME

static _Atomic uint64_t g_counters[SFU_METRIC_COUNT];

#define SFU_METRICS_HASH_CAP 512
#define SFU_METRICS_HASH_MASK (SFU_METRICS_HASH_CAP - 1)

static int s_metric_hash[SFU_METRICS_HASH_CAP];
static _Atomic bool s_metric_hash_inited = false;

static uint32_t metric_hash_str(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619u;
  }
  return h;
}

static void init_metric_hash(void) {
  for (int i = 0; i < SFU_METRICS_HASH_CAP; i++) {
    s_metric_hash[i] = -1;
  }
  for (int i = 0; i < SFU_METRIC_COUNT; i++) {
    uint32_t h = metric_hash_str(k_metric_names[i]);
    uint32_t slot = h & SFU_METRICS_HASH_MASK;
    while (s_metric_hash[slot] != -1) {
      slot = (slot + 1) & SFU_METRICS_HASH_MASK;
    }
    s_metric_hash[slot] = i;
  }
  atomic_store_explicit(&s_metric_hash_inited, true, memory_order_release);
}

static int find_metric(const char *name) {
  if (!name) {
    return -1;
  }
  if (__builtin_expect(!atomic_load_explicit(&s_metric_hash_inited, memory_order_acquire), 0)) {
    init_metric_hash();
  }
  uint32_t h = metric_hash_str(name);
  uint32_t slot = h & SFU_METRICS_HASH_MASK;
  for (int step = 0; step < SFU_METRICS_HASH_CAP; step++) {
    int idx = s_metric_hash[slot];
    if (idx == -1) {
      return -1;
    }
    if (strcmp(k_metric_names[idx], name) == 0) {
      return idx;
    }
    slot = (slot + 1) & SFU_METRICS_HASH_MASK;
  }
  return -1;
}

void sfu_metrics_init(void) {
  if (!atomic_load_explicit(&s_metric_hash_inited, memory_order_acquire)) {
    init_metric_hash();
  }
  for (int i = 0; i < SFU_METRIC_COUNT; i++) {
    atomic_store_explicit(&g_counters[i], 0, memory_order_relaxed);
  }
}

void sfu_metric_inc_id(sfu_metric_id_t id) { sfu_metric_add_id(id, 1); }

void sfu_metric_add_id(sfu_metric_id_t id, uint64_t value) {
  if ((unsigned)id >= SFU_METRIC_COUNT) {
    return;
  }
  atomic_fetch_add_explicit(&g_counters[id], value, memory_order_relaxed);
}

uint64_t sfu_metric_get_id(sfu_metric_id_t id) {
  if ((unsigned)id >= SFU_METRIC_COUNT) {
    return 0;
  }
  return atomic_load_explicit(&g_counters[id], memory_order_relaxed);
}

void sfu_metric_inc(const char *name) { sfu_metric_add(name, 1); }

void sfu_metric_add(const char *name, uint64_t value) {
  int idx = find_metric(name);
  if (idx >= 0) {
    sfu_metric_add_id((sfu_metric_id_t)idx, value);
  }
}

uint64_t sfu_metric_get(const char *name) {
  int idx = find_metric(name);
  return idx < 0 ? 0 : sfu_metric_get_id((sfu_metric_id_t)idx);
}

size_t sfu_metrics_snapshot(char *buf, size_t cap) {
  size_t needed = 0;
  size_t used = 0;

  if (buf && cap > 0) {
    buf[0] = '\0';
  }

  for (int i = 0; i < SFU_METRIC_COUNT; i++) {
    uint64_t v = atomic_load_explicit(&g_counters[i], memory_order_relaxed);
    char line[128];
    int n = snprintf(line, sizeof(line), "%s %llu\n", k_metric_names[i], (unsigned long long)v);
    if (n < 0) {
      continue;
    }
    needed += (size_t)n;

    if (buf && cap > 0 && used + 1 < cap) {
      size_t space = cap - 1 - used; /* leave room for NUL */
      size_t copy = (size_t)n < space ? (size_t)n : space;
      memcpy(buf + used, line, copy);
      used += copy;
      buf[used] = '\0';
    }
  }

  return needed;
}
