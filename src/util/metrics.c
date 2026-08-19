#include "util/metrics.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *const k_metric_names[] = {
    "msg_trunc_drop",
    "json_reject",
    "rtcp_compound_malformed",
    "rtcp_member_unknown",
    "rtcp_twcc_bad",
    "rtcp_nack_bad",
    "rtcp_pli_bad",
    "rtcp_nack_dropped",
    "rtx_build_fail",
    "rtcp_kf_unresolved",
    "twcc_write_fail",
    "egress_protect_fail",
    "egress_protect_replay_old",
    "egress_protect_replay_fail",
    "egress_protect_fail_audio",
    "egress_protect_fail_video",
    "egress_seq_translate_fail",
    "egress_seq_translate_table_full",
    "dtls_restart_detected",
    "dtls_restart_established",
    "dtls_restart_failed",
    "dtls_restart_timeout",
    "dtls_restart_duplicate",
    "ingress_unprotect_fail_rtp",
    "ingress_unprotect_fail_rtcp",
    "ingress_unprotect_auth_fail",
    "ingress_unprotect_replay_fail",
    "ingress_unprotect_replay_old",
    "ingress_unprotect_no_ctx",
    "ingress_unprotect_other",
    "ingress_unprotect_previous_generation",
    "egress_send_full",
    "pacer_dropped_enh",
    "rtx_dropped_budget",
    "audience_rtp_drop",
    "rtp_parse_fail",
    "muted_audio_drop",
    "unnegotiated_rtp_drop",
    "vp9_descriptor_parse_fail",
    "packet_pool_exhausted",
    "packet_meta_pool_exhausted",
    "worker_inbox_full",
    "fanout_ring_full",
    "fanout_job_pool_exhausted",
    "release_queue_full",
    "send_sq_full",
    "egress_admission_drop",
    "egress_output_alloc",
    "egress_copied_bytes",
    "fanout_batch_jobs",
    "fanout_batch_targets",
};

enum { SFU_METRIC_COUNT = sizeof(k_metric_names) / sizeof(k_metric_names[0]) };

static _Atomic uint64_t g_counters[SFU_METRIC_COUNT];

static int find_metric(const char *name) {
  if (!name) {
    return -1;
  }
  for (int i = 0; i < SFU_METRIC_COUNT; i++) {
    if (strcmp(k_metric_names[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

void sfu_metrics_init(void) {
  for (int i = 0; i < SFU_METRIC_COUNT; i++) {
    atomic_store_explicit(&g_counters[i], 0, memory_order_relaxed);
  }
}

void sfu_metric_inc(const char *name) { sfu_metric_add(name, 1); }

void sfu_metric_add(const char *name, uint64_t value) {
  int idx = find_metric(name);
  if (idx < 0) {
    return;
  }
  atomic_fetch_add_explicit(&g_counters[idx], value, memory_order_relaxed);
}

uint64_t sfu_metric_get(const char *name) {
  int idx = find_metric(name);
  if (idx < 0) {
    return 0;
  }
  return atomic_load_explicit(&g_counters[idx], memory_order_relaxed);
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
