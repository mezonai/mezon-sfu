#ifndef SFU_PIPELINE_PACED_SEND_H
#define SFU_PIPELINE_PACED_SEND_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct sfu_worker sfu_worker_t;

#define SFU_PACED_SEND_MAX_PAYLOAD 2048u
#define SFU_PACED_SEND_MIN_BPS 2000000u
#define SFU_PACED_SEND_MAX_DELAY_US 200000LL

typedef struct sfu_paced_send_entry {
  int64_t release_at_us;
  struct sockaddr_storage dst;
  socklen_t dst_len;
  uint16_t len;
  uint8_t data[SFU_PACED_SEND_MAX_PAYLOAD];
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
  bool input_frame_active;
  bool drop_input_frame;
  uint64_t sent;
  uint64_t dropped_full;
  uint64_t dropped_delay_frames;
  uint64_t dropped_frame_packets;
} sfu_paced_send_t;

void sfu_paced_send_init(sfu_paced_send_t *q);
void sfu_paced_send_destroy(sfu_paced_send_t *q);
int64_t sfu_paced_send_projected_delay_us(const sfu_paced_send_t *q, int64_t now_us);
bool sfu_paced_send_admit_frame_packet(sfu_paced_send_t *q, uint32_t rtp_timestamp, bool marker, bool keyframe, int64_t now_us);
bool sfu_paced_send_enqueue(sfu_paced_send_t *q, const uint8_t *data, uint16_t len, const struct sockaddr_storage *dst, socklen_t dst_len, uint32_t pacing_bps,
                            int64_t now_us, int64_t *release_at_us);
bool sfu_paced_send_drain(sfu_paced_send_t *q, sfu_worker_t *w, int64_t now_us);

#endif /* SFU_PIPELINE_PACED_SEND_H */
