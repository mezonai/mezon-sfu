#ifndef SFU_PROTOCOL_SDP_H
#define SFU_PROTOCOL_SDP_H

#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

#define SFU_PT_VP9 98
#define SFU_PT_VP9_RTX 99
#define SFU_PT_AV1 100
#define SFU_PT_AV1_RTX 101
#define SFU_PT_VP8 96
#define SFU_PT_VP8_RTX 97

typedef enum {
  SFU_VIDEO_CODEC_NONE = 0,
  SFU_VIDEO_CODEC_VP8,
  SFU_VIDEO_CODEC_VP9,
  SFU_VIDEO_CODEC_AV1,
} sfu_video_codec_t;

static inline sfu_video_codec_t sfu_video_codec_from_pt(uint8_t pt) {
  switch (pt) {
    case SFU_PT_VP9:
      return SFU_VIDEO_CODEC_VP9;
    case SFU_PT_AV1:
      return SFU_VIDEO_CODEC_AV1;
    case SFU_PT_VP8:
      return SFU_VIDEO_CODEC_VP8;
    default:
      return SFU_VIDEO_CODEC_NONE;
  }
}

int sfu_sdp_build_answer(const sfu_peer_session_t *session, const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag,
                         const char *pwd, const char *fingerprint, char *out, size_t out_cap);

int sfu_sdp_build_offer(const sfu_peer_session_t *session, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                        char *out, size_t out_cap);

int sfu_sdp_build_initial_offer(const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, char *out, size_t out_cap);

#endif /* SFU_PROTOCOL_SDP_H */
