#ifndef SFU_PROTOCOL_SDP_H
#define SFU_PROTOCOL_SDP_H

#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

#define SFU_TWCC_RECV_EXTMAP_ID 6
#define SFU_MID_RECV_EXTMAP_ID 7

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

typedef struct sfu_sdp_receiver_view {
  int64_t publisher_user_id;
  uint32_t publisher_peer_id;
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint32_t screen_ssrc;
  uint32_t screen_rtx_ssrc;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint32_t mid_screen;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  uint8_t screen_pt;
  uint8_t screen_rtx_pt;
  sfu_video_codec_t video_codec;
  sfu_video_codec_t screen_codec;
  bool has_audio;
  bool has_video;
  bool has_screen;
  bool audio_active;
  bool video_active;
  bool screen_active;
  char owner_ufrag[32];
} sfu_sdp_receiver_view_t;

int sfu_sdp_build_answer(sfu_peer_session_t *session, const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag, const char *pwd,
                         const char *fingerprint, char *out, size_t out_cap);

int sfu_sdp_build_offer(sfu_peer_session_t *session, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, char *out,
                        size_t out_cap);

int sfu_sdp_build_initial_offer(const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, bool is_audience, char *out,
                                size_t out_cap);

#endif /* SFU_PROTOCOL_SDP_H */
