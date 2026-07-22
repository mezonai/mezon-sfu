#ifndef SFU_MEDIA_TRANSCEIVER_H
#define SFU_MEDIA_TRANSCEIVER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { SFU_MEDIA_AUDIO, SFU_MEDIA_VIDEO, SFU_MEDIA_SCREEN, SFU_MEDIA_DATA } sfu_media_kind_t;

typedef enum { SFU_DIRECTION_INACTIVE, SFU_DIRECTION_SENDONLY, SFU_DIRECTION_RECVONLY, SFU_DIRECTION_SENDRECV } sfu_direction_t;

typedef struct sfu_transceiver {
  uint16_t mid;
  sfu_media_kind_t kind;
  sfu_direction_t direction;
  bool active;
  uint32_t ssrc;
  uint32_t rtx_ssrc;
  uint8_t payload_type;
  uint8_t rtx_payload_type;
  char stream_id[64];
  char track_id[64];
  char cname[64];
} sfu_transceiver_t;

#endif  // SFU_MEDIA_TRANSCEIVER_H
