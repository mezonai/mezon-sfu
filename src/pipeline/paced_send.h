#ifndef SFU_PIPELINE_PACED_SEND_H
#define SFU_PIPELINE_PACED_SEND_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct sfu_worker sfu_worker_t;
typedef struct sfu_peer_session sfu_peer_session_t;
typedef struct sfu_pacer sfu_pacer_t;

typedef struct sfu_pacer_reservation {
  uint32_t bytes;
  uint8_t pacer_class;
  bool active;
  bool charged;
} sfu_pacer_reservation_t;

#define SFU_PACED_SEND_MAX_PAYLOAD 2048u
#define SFU_PACED_SEND_MAX_DRAIN_PER_SCAN 4u
#define SFU_PACED_SEND_SCAN_INTERVAL_US 2000LL
#define SFU_PACED_SEND_MIN_BPS 2000000u
#define SFU_PACED_SEND_MAX_DELAY_US 200000LL

typedef struct sfu_paced_send_metadata {
  uint64_t assignment_generation;
  uint64_t owner_value;
  uint32_t transport_generation;
  uint32_t address_generation;
  uint32_t remote_slot;
  uint16_t twcc_seq;
  uint16_t subscriber_seq;
  uint32_t media_ssrc;
  uint32_t rtx_ssrc;
  uint32_t egress_generation;
  uint8_t rtx_pt;
  bool twcc_written;
  bool cache_rtx;
} sfu_paced_send_metadata_t;

typedef struct sfu_paced_send_entry {
  int64_t release_at_us;
  int64_t enqueued_at_us;
  struct sockaddr_storage dst;
  socklen_t dst_len;
  uint16_t len;
  uint16_t rtx_plaintext_len;
  uint8_t pacer_class;
  sfu_pacer_t *pacer;
  sfu_pacer_reservation_t reservation;
  sfu_paced_send_metadata_t metadata;
  uint8_t data[SFU_PACED_SEND_MAX_PAYLOAD];
  uint8_t rtx_plaintext[SFU_PACED_SEND_MAX_PAYLOAD];
} sfu_paced_send_entry_t;

typedef struct sfu_paced_send {
  sfu_paced_send_entry_t *entries;
  uint32_t capacity;
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  int64_t next_release_us;
  uint32_t input_timestamp;
  uint32_t high_water;
  int64_t input_frame_started_us;
  int64_t input_frame_base_next_release_us;
  uint32_t input_frame_queued_packets;
  int64_t max_input_frame_span_us;
  int64_t max_enqueue_to_send_us;
  int64_t max_release_late_us;
  uint64_t enqueue_to_send_sum_us;
  uint64_t enqueue_to_send_samples;
  uint64_t input_frames_over_delay;
  uint64_t drain_invocations;
  uint64_t drain_cap_hits;
  uint32_t max_drain_packets;
  bool input_frame_active;
  bool drop_input_frame;
  uint64_t sent;
  uint64_t dropped_full;
  uint64_t dropped_delay_frames;
  uint64_t dropped_frame_packets;
  uint64_t dropped_stale;
} sfu_paced_send_t;

void sfu_paced_send_init(sfu_paced_send_t *q);
void sfu_paced_send_destroy(sfu_paced_send_t *q);
int64_t sfu_paced_send_projected_delay_us(const sfu_paced_send_t *q, int64_t now_us);
bool sfu_paced_send_admit_frame_packet(sfu_paced_send_t *q, uint32_t rtp_timestamp, bool marker, bool keyframe, int64_t now_us);
void sfu_paced_send_reject_input_frame(sfu_paced_send_t *q);
void sfu_paced_send_rollback_input_frame(sfu_paced_send_t *q);
bool sfu_paced_send_enqueue(sfu_paced_send_t *q, const uint8_t *data, uint16_t len, const uint8_t *rtx_plaintext, uint16_t rtx_plaintext_len,
                            const struct sockaddr_storage *dst, socklen_t dst_len, uint8_t pacer_class, uint32_t pacing_bps, sfu_pacer_t *pacer,
                            sfu_pacer_reservation_t *reservation, const sfu_paced_send_metadata_t *metadata, int64_t now_us, int64_t *release_at_us);
bool sfu_paced_send_drain(sfu_paced_send_t *q, sfu_worker_t *w, sfu_peer_session_t *session, int64_t now_us);

#endif /* SFU_PIPELINE_PACED_SEND_H */
