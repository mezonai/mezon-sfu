#ifndef SFU_SVC_DESCRIPTOR_H
#define SFU_SVC_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol/signaling/sdp.h"

/*
 * Codec-agnostic SVC view of one RTP payload.
 *
 * The fields mirror what the layer scheduler actually decides on; both the
 * VP9 payload descriptor and AV1's Dependency Descriptor map onto them:
 *
 *   sid     spatial layer the packet belongs to
 *   tid     temporal layer the packet belongs to
 *   p_bit   0 marks the start of a frame (VP9: P bit inverted)
 *   u_bit   1 marks a temporal-layer switch point (VP9: U bit; AV1 DD:
 *           decodable-with-lower-tid chains)
 *   d_bit   1 marks an inter-layer dependency (VP9: D bit)
 */
typedef struct sfu_svc_descriptor {
  uint8_t sid;
  uint8_t tid;
  uint8_t p_bit;
  uint8_t u_bit;
  uint8_t d_bit;
} sfu_svc_descriptor_t;

/* Parses the payload's scalability structure for the given codec.
 * Returns 0 on success, -1 if the codec has no SVC parser or the payload
 * is malformed. */
int sfu_svc_parse_descriptor(sfu_video_codec_t codec, const uint8_t *payload, size_t payload_len, sfu_svc_descriptor_t *out);

/* VP9: a keyframe starts with a non-inter-frame (P=0) picture on the base
 * spatial layer. Used by the scheduler's needs_keyframe gate. */
static inline bool sfu_svc_descriptor_is_keyframe(const sfu_svc_descriptor_t *d) { return d->p_bit == 0 && d->sid == 0; }

#endif /* SFU_SVC_DESCRIPTOR_H */
