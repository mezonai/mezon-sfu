#ifndef SFU_MEDIA_SOURCE_H
#define SFU_MEDIA_SOURCE_H

#include <stdint.h>

struct sfu_peer;

typedef struct sfu_media_source {
  uint32_t id;

  struct sfu_peer *owner;

  uint32_t audio_ssrc;

  uint32_t video_ssrc;

  uint32_t rtx_ssrc;

} sfu_media_source_t;

#endif  // SFU_MEDIA_SOURCE_H
