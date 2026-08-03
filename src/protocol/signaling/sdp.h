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

/* Read-only view over one receiver-snapshot entry used by the SDP builders.
 * Plain value type (no ownership); the SDP caller must hold the snapshot it
 * was filled from, or be running under the room lock. */
typedef struct sfu_sdp_receiver_view {
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  bool has_audio;
  bool has_video;
  bool audio_active;
  bool video_active;
  char owner_ufrag[32];
} sfu_sdp_receiver_view_t;

/* Lifetime invariants for the session-taking SDP builders below (F-18):
 * the caller must guarantee that `session` (and therefore session->cold)
 * stays alive for the whole call, either by holding a refcounted pin
 * (sfu_session_release discipline) or by holding the session's room lock.
 * The builders only read immutable or caller-pinned state: the receiver
 * set is traversed exclusively through a retained snapshot acquired at
 * entry (sfu_session_receivers_acquire / sfu_receiver_snapshot_release);
 * the only mutable session fields read are uplink_video.payload_type /
 * rtx_payload_type, which are benign best-effort negotiation defaults
 * (any concurrent value yields a valid SDP). They never follow snapshot
 * entries back into owner/session pointers. */
int sfu_sdp_build_answer(const sfu_peer_session_t *session, const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag,
                         const char *pwd, const char *fingerprint, char *out, size_t out_cap);

int sfu_sdp_build_offer(const sfu_peer_session_t *session, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                        char *out, size_t out_cap);

int sfu_sdp_build_initial_offer(const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, char *out, size_t out_cap);

#endif /* SFU_PROTOCOL_SDP_H */
