#ifndef SFU_CONGESTION_PROBE_CONTROLLER_H
#define SFU_CONGESTION_PROBE_CONTROLLER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "congestion/gcc.h"

typedef struct sfu_peer_session sfu_peer_session_t;
typedef struct sfu_worker sfu_worker_t;

#define SFU_PROBE_CLUSTER_DURATION_US 15000LL
#define SFU_PROBE_CLUSTER_TIMEOUT_US 500000LL
#define SFU_PROBE_MIN_STEP_BPS 500000u
#define SFU_PROBE_MAX_BITRATE_BPS 5000000u
#define SFU_PROBE_COOLDOWN_SUCCESS_US 250000LL
#define SFU_PROBE_COOLDOWN_FAILURE_US 1000000LL
#define SFU_PROBE_SUCCESS_THRESHOLD_NUM 85u
#define SFU_PROBE_SUCCESS_THRESHOLD_DEN 100u

typedef enum sfu_probe_state {
  SFU_PROBE_STATE_IDLE = 0,
  SFU_PROBE_STATE_PROBING,
  SFU_PROBE_STATE_WAITING_FEEDBACK,
} sfu_probe_state_t;

typedef struct sfu_probe_controller {
  pthread_mutex_t lock;
  sfu_probe_state_t state;
  uint32_t cluster_id;
  uint32_t base_bitrate_bps;
  uint32_t target_bitrate_bps;
  uint32_t bytes_target;
  uint32_t bytes_sent;
  uint32_t packets_sent;
  uint32_t packets_received;
  uint32_t bytes_received;
  uint32_t packets_lost;
  int64_t start_time_us;
  int64_t send_end_time_us;
  int64_t first_recv_us;
  int64_t last_recv_us;
  int64_t cooldown_until_us;
  bool has_unmet_demand;
  bool aborted;
  uint64_t clusters_started;
  uint64_t clusters_succeeded;
  uint64_t clusters_failed;
  uint64_t clusters_aborted;
} sfu_probe_controller_t;

sfu_probe_controller_t *sfu_probe_controller_create(void);
void sfu_probe_controller_init(sfu_probe_controller_t *pc);
void sfu_probe_controller_destroy(sfu_probe_controller_t *pc);
void sfu_probe_controller_reset(sfu_probe_controller_t *pc);

void sfu_probe_controller_set_unmet_demand(sfu_probe_controller_t *pc, bool has_unmet_demand);
bool sfu_probe_controller_is_probing(sfu_probe_controller_t *pc);

bool sfu_probe_controller_should_probe(sfu_probe_controller_t *pc, const gcc_bwe_context_t *gcc, int64_t now_us,
                                       bool twcc_negotiated, bool rtx_available,
                                       uint32_t rtx_queue_count, uint32_t media_backlog_count);
bool sfu_probe_controller_start_probe(sfu_probe_controller_t *pc, gcc_bwe_context_t *gcc, int64_t now_us);

void sfu_probe_controller_on_packet_received(sfu_probe_controller_t *pc, uint32_t cluster_id, uint32_t size_bytes,
                                             int64_t send_time_us, int64_t recv_time_us);
void sfu_probe_controller_on_packet_lost(sfu_probe_controller_t *pc, uint32_t cluster_id);
void sfu_probe_controller_abort(sfu_probe_controller_t *pc, gcc_bwe_context_t *gcc, int64_t now_us, const char *reason);

bool sfu_probe_controller_check_cluster_done(sfu_probe_controller_t *pc, gcc_bwe_context_t *gcc, int64_t now_us,
                                             uint32_t *out_probe_bitrate_bps);

void sfu_probe_controller_step(sfu_peer_session_t *session, sfu_worker_t *w, int64_t now_us);

#endif /* SFU_CONGESTION_PROBE_CONTROLLER_H */
