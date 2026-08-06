#ifndef SFU_SVC_DESCRIPTOR_H
#define SFU_SVC_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

typedef struct sfu_svc_descriptor {
  uint8_t sid;
  uint8_t tid;
  uint8_t p_bit;
  uint8_t u_bit;
  uint8_t d_bit;
} sfu_svc_descriptor_t;

int sfu_svc_parse_descriptor(sfu_video_codec_t codec, const uint8_t *payload, size_t payload_len, sfu_svc_descriptor_t *out);

static inline bool sfu_svc_descriptor_is_keyframe(const sfu_svc_descriptor_t *d) { return d->p_bit == 0 && d->sid == 0; }

#endif /* SFU_SVC_DESCRIPTOR_H */
