#include "protocol/signaling/sdp.h"
#include "peer/session.h"
#include "util/log.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int starts_with(const char *s, size_t len, const char *prefix) {
  size_t plen = strlen(prefix);
  return len >= plen && memcmp(s, prefix, plen) == 0;
}

static const char *SKIP_PREFIXES[] = {"a=ice-ufrag",   "a=ice-pwd",           "a=fingerprint", "a=setup", "a=candidate", "c=IN",
                                      "a=ice-options", "a=end-of-candidates", "a=rtcp:",       "a=msid",  "a=ssrc",      NULL};

static int should_skip_line(const char *line, size_t len) {
  for (int i = 0; SKIP_PREFIXES[i]; i++) {
    if (starts_with(line, len, SKIP_PREFIXES[i])) {
      return 1;
    }
  }
  return 0;
}

static int append_line_n(char *out, size_t out_cap, size_t *offset, const char *text, size_t len) {
  if (*offset + len + 2 >= out_cap) {
    return -1;
  }
  memcpy(out + *offset, text, len);
  *offset += len;
  out[(*offset)++] = '\r';
  out[(*offset)++] = '\n';
  return 0;
}

static int append_line(char *out, size_t out_cap, size_t *offset, const char *text) { return append_line_n(out, out_cap, offset, text, strlen(text)); }

static int append_media_transport_headers(char *out, size_t out_cap, size_t *offset, const char *host, uint16_t port, const char *ufrag, const char *pwd,
                                          const char *fingerprint) {
  char attr[512];
  int n;
  n = snprintf(attr, sizeof(attr), "c=IN IP4 %s", host);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(attr, sizeof(attr), "a=ice-ufrag:%s", ufrag);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(attr, sizeof(attr), "a=ice-pwd:%s", pwd);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(attr, sizeof(attr), "a=fingerprint:sha-256 %s", fingerprint);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, offset, "a=setup:passive") != 0) {
    return -1;
  }
  n = snprintf(attr, sizeof(attr), "a=candidate:1 1 udp 2130706431 %s %u typ host", host, port);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, offset, "a=end-of-candidates") != 0) {
    return -1;
  }
  return 0;
}

static int append_twcc_attributes(char *out, size_t out_cap, size_t *offset);
static int append_mid_recv_attribute(char *out, size_t out_cap, size_t *offset);
static int append_video_codec_attributes(char *out, size_t out_cap, size_t *offset, sfu_video_codec_t codec, uint8_t video_pt, uint8_t rtx_pt);
static int append_remote_audio_msid(char *out, size_t out_cap, size_t *offset, int64_t user_id, uint32_t peer_id);
static int append_remote_video_msid(char *out, size_t out_cap, size_t *offset, int64_t user_id, uint32_t peer_id, const char *track_prefix);

static int append_bundled_transport_headers(char *out, size_t out_cap, size_t *offset, const char *host, const char *ufrag, const char *pwd,
                                            const char *fingerprint) {
  char attr[512];
  int n = snprintf(attr, sizeof(attr), "c=IN IP4 %s", host);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(attr, sizeof(attr), "a=ice-ufrag:%s", ufrag);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(attr, sizeof(attr), "a=ice-pwd:%s", pwd);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(attr, sizeof(attr), "a=fingerprint:sha-256 %s", fingerprint);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }
  return append_line(out, out_cap, offset, "a=setup:passive");
}

static int append_fragment(char *out, size_t out_cap, size_t *offset, const char *fragment, size_t fragment_len) {
  if (*offset + fragment_len >= out_cap) {
    return -1;
  }
  memcpy(out + *offset, fragment, fragment_len);
  *offset += fragment_len;
  return 0;
}

static int append_bundle_group(char *out, size_t out_cap, size_t *offset, uint32_t next_remote_mid) {
  if (*offset + 20 >= out_cap) {
    return -1;
  }
  int n = snprintf(out + *offset, out_cap - *offset, "a=group:BUNDLE 0 1 2");
  if (n < 0 || (size_t)n >= out_cap - *offset) {
    return -1;
  }
  *offset += (size_t)n;
  for (uint32_t mid_audio = SFU_REMOTE_MID_BASE; mid_audio < next_remote_mid; mid_audio += SFU_REMOTE_TRANSCEIVERS_PER_SLOT) {
    n = snprintf(out + *offset, out_cap - *offset, " %u %u %u", mid_audio, mid_audio + 1, mid_audio + 2);
    if (n < 0 || (size_t)n >= out_cap - *offset) {
      return -1;
    }
    *offset += (size_t)n;
  }
  if (*offset + 2 >= out_cap) {
    return -1;
  }
  out[(*offset)++] = '\r';
  out[(*offset)++] = '\n';
  return 0;
}

static int append_bundled_audio(char *out, size_t out_cap, size_t *offset, uint16_t port, const char *transport, size_t transport_len, uint32_t mid, bool live,
                                const sfu_sdp_receiver_view_t *slot) {
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, offset, buf, (size_t)n) != 0) {
    return -1;
  }
  if (append_fragment(out, out_cap, offset, transport, transport_len) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, offset, live ? "a=sendonly" : "a=inactive") != 0) {
    return -1;
  }
  n = snprintf(buf, sizeof(buf), "a=mid:%u", mid);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, offset, buf, (size_t)n) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, offset, "a=rtcp-mux") != 0) {
    return -1;
  }
  if (append_mid_recv_attribute(out, out_cap, offset) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, offset, "a=rtpmap:111 opus/48000/2") != 0) {
    return -1;
  }
  if (live && (!slot || append_remote_audio_msid(out, out_cap, offset, slot->publisher_user_id, slot->publisher_peer_id) != 0)) {
    return -1;
  }
  return 0;
}

static int append_bundled_video(char *out, size_t out_cap, size_t *offset, uint16_t port, const char *transport, size_t transport_len, uint32_t mid, bool live,
                                const sfu_sdp_receiver_view_t *slot, uint8_t fallback_pt, uint8_t fallback_rtx_pt, bool screen) {
  char buf[256];
  uint8_t video_pt = (live && slot && (screen ? slot->screen_pt : slot->video_pt) != 0) ? (screen ? slot->screen_pt : slot->video_pt) : fallback_pt;
  uint8_t rtx_pt =
      (live && slot && (screen ? slot->screen_rtx_pt : slot->video_rtx_pt) != 0) ? (screen ? slot->screen_rtx_pt : slot->video_rtx_pt) : fallback_rtx_pt;
  int n;
  if (rtx_pt != 0) {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, video_pt, rtx_pt);
  } else {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, video_pt ? video_pt : SFU_PT_VP8);
  }
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, offset, buf, (size_t)n) != 0) {
    return -1;
  }
  if (append_fragment(out, out_cap, offset, transport, transport_len) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, offset, live ? "a=sendonly" : "a=inactive") != 0) {
    return -1;
  }
  n = snprintf(buf, sizeof(buf), "a=mid:%u", mid);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, offset, buf, (size_t)n) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, offset, "a=rtcp-mux") != 0) {
    return -1;
  }
  if (append_mid_recv_attribute(out, out_cap, offset) != 0) {
    return -1;
  }
  if (!live) {
    sfu_video_codec_t codec = SFU_VIDEO_CODEC_NONE;
    if (slot) {
      codec = screen ? slot->screen_codec : slot->video_codec;
    }
    if (codec == SFU_VIDEO_CODEC_NONE) {
      codec = sfu_video_codec_from_pt(video_pt);
    }
    if (codec == SFU_VIDEO_CODEC_NONE) {
      codec = SFU_VIDEO_CODEC_VP8;
    }
    const char *codec_name = (codec == SFU_VIDEO_CODEC_VP9) ? "VP9" : (codec == SFU_VIDEO_CODEC_AV1) ? "AV1" : "VP8";
    char attr[128];
    n = snprintf(attr, sizeof(attr), "a=rtpmap:%u %s/90000", video_pt, codec_name);
    if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
      return -1;
    }
    if (rtx_pt != 0) {
      n = snprintf(attr, sizeof(attr), "a=rtpmap:%u rtx/90000", rtx_pt);
      if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
        return -1;
      }
      n = snprintf(attr, sizeof(attr), "a=fmtp:%u apt=%u", rtx_pt, video_pt);
      if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
        return -1;
      }
    }
    return 0;
  }
  if (append_twcc_attributes(out, out_cap, offset) != 0) {
    return -1;
  }
  sfu_video_codec_t codec = screen ? slot->screen_codec : slot->video_codec;
  if (append_video_codec_attributes(out, out_cap, offset, codec, video_pt, rtx_pt) != 0) {
    return -1;
  }
  if (append_remote_video_msid(out, out_cap, offset, slot->publisher_user_id, slot->publisher_peer_id, screen ? "screen" : "video") != 0) {
    return -1;
  }
  return 0;
}

#define SFU_TWCC_LOCAL_EXTMAP_ID 6

static int append_twcc_attributes(char *out, size_t out_cap, size_t *offset) {
  char line[160];
  int n = snprintf(line, sizeof(line), "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", SFU_TWCC_LOCAL_EXTMAP_ID);
  if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
    return -1;
  }
  return 0;
}

static int append_twcc_recv_attribute(char *out, size_t out_cap, size_t *offset) {
  char line[192];
  int n = snprintf(line, sizeof(line), "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", SFU_TWCC_RECV_EXTMAP_ID);
  if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
    return -1;
  }
  return 0;
}

static int append_mid_recv_attribute(char *out, size_t out_cap, size_t *offset) {
  char line[128];
  int n = snprintf(line, sizeof(line), "a=extmap:%d urn:ietf:params:rtp-hdrext:sdes:mid", SFU_MID_RECV_EXTMAP_ID);
  return n < 0 || (size_t)n >= sizeof(line) ? -1 : append_line_n(out, out_cap, offset, line, (size_t)n);
}

static int append_video_codec_attributes(char *out, size_t out_cap, size_t *offset, sfu_video_codec_t codec, uint8_t video_pt, uint8_t rtx_pt) {
  if (codec == SFU_VIDEO_CODEC_NONE) {
    codec = sfu_video_codec_from_pt(video_pt);
  }
  const char *codec_name = (codec == SFU_VIDEO_CODEC_VP9) ? "VP9" : (codec == SFU_VIDEO_CODEC_AV1) ? "AV1" : "VP8";

  char line[128];
  int n;
  n = snprintf(line, sizeof(line), "a=rtpmap:%u %s/90000", video_pt, codec_name);
  if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(line, sizeof(line), "a=rtcp-fb:%u nack", video_pt);
  if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(line, sizeof(line), "a=rtcp-fb:%u nack pli", video_pt);
  if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
    return -1;
  }
  n = snprintf(line, sizeof(line), "a=rtcp-fb:%u transport-cc", video_pt);
  if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
    return -1;
  }
  if (codec == SFU_VIDEO_CODEC_VP9) {
    n = snprintf(line, sizeof(line), "a=fmtp:%u profile-id=0", video_pt);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
  }
  if (rtx_pt != 0) {
    n = snprintf(line, sizeof(line), "a=rtpmap:%u rtx/90000", rtx_pt);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    n = snprintf(line, sizeof(line), "a=fmtp:%u apt=%u", rtx_pt, video_pt);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
  }
  return 0;
}

static int append_remote_audio_msid(char *out, size_t out_cap, size_t *offset, int64_t user_id, uint32_t peer_id) {
  char line[160];
  char stream_id[64];
  int n = snprintf(stream_id, sizeof(stream_id), "u%" PRId64 "-p%u", user_id, peer_id);
  if (n < 0 || (size_t)n >= sizeof(stream_id)) {
    return -1;
  }
  n = snprintf(line, sizeof(line), "a=msid:%s audio-%s", stream_id, stream_id);
  return n < 0 || (size_t)n >= sizeof(line) ? -1 : append_line_n(out, out_cap, offset, line, (size_t)n);
}

static int append_remote_video_msid(char *out, size_t out_cap, size_t *offset, int64_t user_id, uint32_t peer_id, const char *track_prefix) {
  char line[160];
  char stream_id[64];
  int n = snprintf(stream_id, sizeof(stream_id), "u%" PRId64 "-p%u", user_id, peer_id);
  if (n < 0 || (size_t)n >= sizeof(stream_id)) {
    return -1;
  }
  if (!track_prefix) {
    track_prefix = "video";
  }
  n = snprintf(line, sizeof(line), "a=msid:%s %s-%s", stream_id, track_prefix, stream_id);
  return n < 0 || (size_t)n >= sizeof(line) ? -1 : append_line_n(out, out_cap, offset, line, (size_t)n);
}

int sfu_sdp_build_initial_offer(const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, bool is_audience, char *out,
                                size_t out_cap) {
  size_t off = 0;
  char buf[512];
  int n;

  if (append_line(out, out_cap, &off, "v=0") != 0) {
    return -1;
  }

  n = snprintf(buf, sizeof(buf), "o=- %ld 2 IN IP4 %s", (long)time(NULL), host);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "s=-") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "t=0 0") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=group:BUNDLE 0 1 2") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=ice-lite") != 0) {
    return -1;
  }

  n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    return -1;
  }

  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, is_audience ? "a=inactive" : "a=recvonly") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=mid:0") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    return -1;
  }

  if (append_twcc_recv_attribute(out, out_cap, &off) != 0 || append_mid_recv_attribute(out, out_cap, &off) != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
    return -1;
  }

  n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u %u %u %u %u", port, SFU_PT_VP9, SFU_PT_VP9_RTX, SFU_PT_AV1, SFU_PT_AV1_RTX, SFU_PT_VP8,
               SFU_PT_VP8_RTX);

  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    return -1;
  }

  if (append_bundled_transport_headers(out, out_cap, &off, host, ufrag, pwd, fingerprint) != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, is_audience ? "a=inactive" : "a=recvonly") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=mid:1") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    return -1;
  }

  if (append_twcc_recv_attribute(out, out_cap, &off) != 0 || append_mid_recv_attribute(out, out_cap, &off) != 0) {
    return -1;
  }

  if (append_video_codec_attributes(out, out_cap, &off, SFU_VIDEO_CODEC_VP9, SFU_PT_VP9, SFU_PT_VP9_RTX) != 0) {
    return -1;
  }
  if (append_video_codec_attributes(out, out_cap, &off, SFU_VIDEO_CODEC_AV1, SFU_PT_AV1, SFU_PT_AV1_RTX) != 0) {
    return -1;
  }
  if (append_video_codec_attributes(out, out_cap, &off, SFU_VIDEO_CODEC_VP8, SFU_PT_VP8, SFU_PT_VP8_RTX) != 0) {
    return -1;
  }

  n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u %u %u %u %u", port, SFU_PT_VP9, SFU_PT_VP9_RTX, SFU_PT_AV1, SFU_PT_AV1_RTX, SFU_PT_VP8,
               SFU_PT_VP8_RTX);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0 ||
      append_bundled_transport_headers(out, out_cap, &off, host, ufrag, pwd, fingerprint) != 0 ||
      append_line(out, out_cap, &off, is_audience ? "a=inactive" : "a=recvonly") != 0 || append_line(out, out_cap, &off, "a=mid:2") != 0 ||
      append_line(out, out_cap, &off, "a=rtcp-mux") != 0 || append_twcc_recv_attribute(out, out_cap, &off) != 0 ||
      append_mid_recv_attribute(out, out_cap, &off) != 0 ||
      append_video_codec_attributes(out, out_cap, &off, SFU_VIDEO_CODEC_VP9, SFU_PT_VP9, SFU_PT_VP9_RTX) != 0 ||
      append_video_codec_attributes(out, out_cap, &off, SFU_VIDEO_CODEC_AV1, SFU_PT_AV1, SFU_PT_AV1_RTX) != 0 ||
      append_video_codec_attributes(out, out_cap, &off, SFU_VIDEO_CODEC_VP8, SFU_PT_VP8, SFU_PT_VP8_RTX) != 0) {
    return -1;
  }

  return (int)off;
}

static bool sfu_sdp_receiver_view(const sfu_receiver_snapshot_t *snap, uint32_t i, sfu_sdp_receiver_view_t *view) {
  if (!snap) {
    return false;
  }
  const sfu_receiver_entry_t *e = &snap->entries[i];
  if (!e->has_audio && !e->has_video && !e->has_screen) {
    return false;
  }
  view->publisher_user_id = e->publisher_user_id;
  view->publisher_peer_id = e->publisher_peer_id;
  view->audio_ssrc = e->audio_ssrc;
  view->video_ssrc = e->video_ssrc;
  view->video_rtx_ssrc = e->video_rtx_ssrc;
  view->screen_ssrc = e->screen_ssrc;
  view->screen_rtx_ssrc = e->screen_rtx_ssrc;
  view->mid_audio = e->mid_audio;
  view->mid_video = e->mid_video;
  view->mid_screen = e->mid_screen;
  view->video_pt = e->video_pt;
  view->video_rtx_pt = e->video_rtx_pt;
  view->screen_pt = e->screen_pt;
  view->screen_rtx_pt = e->screen_rtx_pt;
  view->video_codec = e->video_codec;
  view->screen_codec = e->screen_codec;
  view->has_audio = e->has_audio;
  view->has_video = e->has_video;
  view->has_screen = e->has_screen;
  view->audio_active = e->audio_active;
  view->video_active = e->video_active;
  view->screen_active = e->screen_active;
  snprintf(view->owner_ufrag, sizeof(view->owner_ufrag), "%s", e->subscriber_ufrag);
  return true;
}

int sfu_sdp_build_answer(sfu_peer_session_t *session, const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag, const char *pwd,
                         const char *fingerprint, char *out, size_t out_cap) {
  size_t off = 0;
  int in_media = 0;
  int saw_media_line = 0;
  int current_media = 0;

  assert(session != NULL);

  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(session);
  uint32_t receiver_count = snap ? snap->count : 0;

  uint8_t video_pt = session->uplink_video.payload_type ? session->uplink_video.payload_type : SFU_PT_VP8;
  uint8_t rtx_pt = session->uplink_video.rtx_payload_type ? session->uplink_video.rtx_payload_type : SFU_PT_VP8_RTX;

  size_t pos = 0;
  while (pos < offer_len) {
    size_t line_start = pos;
    while (pos < offer_len && offer[pos] != '\n') {
      pos++;
    }
    size_t line_end = pos;
    if (line_end > line_start && offer[line_end - 1] == '\r') {
      line_end--;
    }
    if (pos < offer_len) {
      pos++;
    }

    const char *line = offer + line_start;
    size_t len = line_end - line_start;
    if (len == 0) {
      continue;
    }

    if (starts_with(line, len, "a=group:BUNDLE")) {
      char bundle_line[512];
      size_t blen = len < sizeof(bundle_line) - 1 ? len : sizeof(bundle_line) - 1;
      memcpy(bundle_line, line, blen);
      bundle_line[blen] = '\0';

      uint32_t tmp_mid = 100;
      for (uint32_t i = 0; i < receiver_count; i++) {
        sfu_sdp_receiver_view_t slot;
        if (!sfu_sdp_receiver_view(snap, i, &slot)) {
          continue;
        }

        if (slot.has_audio && slot.audio_ssrc != 0) {
          size_t curr_len = strlen(bundle_line);
          snprintf(bundle_line + curr_len, sizeof(bundle_line) - curr_len, " %u", tmp_mid++);
        }
        if (slot.has_video && slot.video_ssrc != 0) {
          size_t curr_len = strlen(bundle_line);
          snprintf(bundle_line + curr_len, sizeof(bundle_line) - curr_len, " %u", tmp_mid++);
        }
      }
      if (append_line(out, out_cap, &off, bundle_line) != 0) {
        goto fail;
      }
      continue;
    }

    if (starts_with(line, len, "o=")) {
      char o_line[128];
      int n = snprintf(o_line, sizeof(o_line), "o=- %ld 2 IN IP4 %s", (long)time(NULL), host);
      if (n < 0 || (size_t)n >= sizeof(o_line) || append_line_n(out, out_cap, &off, o_line, (size_t)n) != 0) {
        goto fail;
      }
      continue;
    }

    bool audience_local_media = atomic_load_explicit(&session->is_audience, memory_order_acquire) && (current_media == 1 || current_media == 2);
    if (starts_with(line, len, "a=sendonly")) {
      if (append_line(out, out_cap, &off, audience_local_media ? "a=inactive" : "a=recvonly") != 0) {
        goto fail;
      }
      continue;
    }
    if (starts_with(line, len, "a=recvonly")) {
      if (append_line(out, out_cap, &off, audience_local_media ? "a=inactive" : "a=sendonly") != 0) {
        goto fail;
      }
      continue;
    }
    if (starts_with(line, len, "a=sendrecv")) {
      if (append_line(out, out_cap, &off, audience_local_media ? "a=inactive" : "a=sendrecv") != 0) {
        goto fail;
      }
      continue;
    }
    if (starts_with(line, len, "a=inactive")) {
      if (append_line(out, out_cap, &off, "a=inactive") != 0) {
        goto fail;
      }
      continue;
    }

    if (starts_with(line, len, "m=")) {
      saw_media_line = 1;
      in_media = 1;

      if (starts_with(line, len, "m=audio")) {
        current_media = 1;
      } else if (starts_with(line, len, "m=video")) {
        current_media = 2;
      } else {
        current_media = 0;
      }

      const char *sp1 = memchr(line, ' ', len);
      if (!sp1) {
        goto fail;
      }
      const char *sp2 = memchr(sp1 + 1, ' ', len - (size_t)(sp1 + 1 - line));
      if (!sp2) {
        goto fail;
      }

      char m_line[256];
      int m_len;
      const char *sp3 = memchr(sp2 + 1, ' ', len - (size_t)(sp2 + 1 - line));
      if (current_media == 2 && video_pt != 0 && sp3 != NULL) {
        if (rtx_pt != 0) {
          m_len = snprintf(m_line, sizeof(m_line), "%.*s %u%.*s %u %u", (int)(sp1 - line), line, port, (int)(sp3 - sp2), sp2, video_pt, rtx_pt);
        } else {
          m_len = snprintf(m_line, sizeof(m_line), "%.*s %u%.*s %u", (int)(sp1 - line), line, port, (int)(sp3 - sp2), sp2, video_pt);
        }
      } else {
        m_len = snprintf(m_line, sizeof(m_line), "%.*s %u%.*s", (int)(sp1 - line), line, port, (int)(len - (size_t)(sp2 - line)), sp2);
      }
      if (m_len < 0 || (size_t)m_len >= sizeof(m_line) || append_line_n(out, out_cap, &off, m_line, (size_t)m_len) != 0) {
        goto fail;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        goto fail;
      }
      if (current_media == 2 && video_pt != 0) {
        if (append_video_codec_attributes(out, out_cap, &off, session->uplink_video.codec, video_pt, rtx_pt) != 0) {
          goto fail;
        }
      }
      continue;
    }

    if (!in_media) {
      if (should_skip_line(line, len)) {
        continue;
      }
      if (append_line_n(out, out_cap, &off, line, len) != 0) {
        goto fail;
      }
    } else {
      if (starts_with(line, len, "a=ssrc:")) {
        uint32_t parsed_ssrc = 0;
        if (sscanf(line, "a=ssrc:%u", &parsed_ssrc) == 1) {
          if (current_media == 1 && session->uplink_audio.ssrc == 0) {
            session->uplink_audio.ssrc = parsed_ssrc;
          } else if (current_media == 2 && session->uplink_video.ssrc == 0) {
            session->uplink_video.ssrc = parsed_ssrc;
          }
        }
      }

      if (should_skip_line(line, len)) {
        continue;
      }
      if (current_media == 2 && video_pt != 0) {
        if (starts_with(line, len, "a=rtpmap:") || starts_with(line, len, "a=rtcp-fb:") || starts_with(line, len, "a=fmtp:")) {
          continue;
        }
      }
      if (append_line_n(out, out_cap, &off, line, len) != 0) {
        goto fail;
      }
    }
  }

  if (!saw_media_line) {
    SFU_LOG_WARN("SDP offer had no m= line, cannot build an answer");
    goto fail;
  }

  /* An SDP answer may only answer media sections present in the offer.
   * Remote subscriptions are introduced later by sfu_sdp_build_offer(). */

  if (snap) {
    sfu_subscriptions_snapshot_release(snap);
  }
  return (int)off;

fail:
  if (snap) {
    sfu_subscriptions_snapshot_release(snap);
  }
  return -1;
}

int sfu_sdp_build_offer(sfu_peer_session_t *session, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, char *out,
                        size_t out_cap) {
  size_t off = 0;
  char buf[512];
  int n;

  assert(session != NULL);

  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(session);
  uint32_t receiver_count = snap ? snap->count : 0;
  char bundled_transport[512];
  size_t bundled_transport_len = 0;
  if (append_bundled_transport_headers(bundled_transport, sizeof(bundled_transport), &bundled_transport_len, host, ufrag, pwd, fingerprint) != 0) {
    goto fail;
  }
  int16_t mid_index[SFU_MAX_REMOTE_SLOTS];
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    mid_index[i] = -1;
  }
  for (uint32_t i = 0; i < receiver_count; i++) {
    uint32_t mid = snap->entries[i].mid_audio;
    if (mid < SFU_REMOTE_MID_BASE || ((mid - SFU_REMOTE_MID_BASE) % SFU_REMOTE_TRANSCEIVERS_PER_SLOT) != 0) {
      SFU_LOG_WARN("SDP_BUILD: invalid audio mid %u", mid);
      goto fail;
    }
    uint32_t slot = (mid - SFU_REMOTE_MID_BASE) / SFU_REMOTE_TRANSCEIVERS_PER_SLOT;
    if (slot >= SFU_MAX_REMOTE_SLOTS || mid_index[slot] >= 0) {
      SFU_LOG_WARN("SDP_BUILD: duplicate/out-of-range audio mid %u", mid);
      goto fail;
    }
    mid_index[slot] = (int16_t)i;
  }
  bool is_audience = atomic_load_explicit(&session->is_audience, memory_order_acquire);
  uint8_t local_video_pt = 0;
  uint8_t local_rtx_pt = 0;
  uint8_t local_screen_pt = 0;
  uint8_t local_screen_rtx_pt = 0;
  sfu_video_codec_t local_video_codec = SFU_VIDEO_CODEC_NONE;
  sfu_video_codec_t local_screen_codec = SFU_VIDEO_CODEC_NONE;
  pthread_mutex_lock(&session->media_lock);
  local_video_pt = session->uplink_video.payload_type ? session->uplink_video.payload_type : SFU_PT_VP8;
  local_rtx_pt = session->uplink_video.rtx_payload_type ? session->uplink_video.rtx_payload_type : SFU_PT_VP8_RTX;
  local_video_codec = session->uplink_video.codec;
  local_screen_pt = session->screen.payload_type ? session->screen.payload_type : local_video_pt;
  local_screen_rtx_pt = session->screen.rtx_payload_type ? session->screen.rtx_payload_type : local_rtx_pt;
  local_screen_codec = session->screen.codec != SFU_VIDEO_CODEC_NONE ? session->screen.codec : local_video_codec;
  pthread_mutex_unlock(&session->media_lock);

  if (append_line(out, out_cap, &off, "v=0") != 0) {
    goto fail;
  }
  n = snprintf(buf, sizeof(buf), "o=- %ld 2 IN IP4 %s", (long)time(NULL), host);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "s=-") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "t=0 0") != 0) {
    goto fail;
  }

  SFU_LOG_DEBUG("SDP_BUILD: ufrag=%s receiver_count=%u", ufrag ? ufrag : "unknown", receiver_count);

  uint32_t next_remote_mid = atomic_load_explicit(&session->next_remote_mid, memory_order_acquire);
  if (append_bundle_group(out, out_cap, &off, next_remote_mid) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=ice-lite") != 0) {
    goto fail;
  }

  n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    goto fail;
  }
  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, is_audience ? "a=inactive" : "a=recvonly") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=mid:0") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    goto fail;
  }
  if (append_twcc_recv_attribute(out, out_cap, &off) != 0 || append_mid_recv_attribute(out, out_cap, &off) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
    goto fail;
  }

  if (local_rtx_pt != 0) {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, local_video_pt, local_rtx_pt);
  } else {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, local_video_pt);
  }
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    goto fail;
  }
  if (append_fragment(out, out_cap, &off, bundled_transport, bundled_transport_len) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, is_audience ? "a=inactive" : "a=recvonly") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=mid:1") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    goto fail;
  }
  if (append_twcc_recv_attribute(out, out_cap, &off) != 0 || append_mid_recv_attribute(out, out_cap, &off) != 0) {
    goto fail;
  }
  if (append_video_codec_attributes(out, out_cap, &off, local_video_codec, local_video_pt, local_rtx_pt) != 0) {
    goto fail;
  }

  if (local_screen_rtx_pt != 0) {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, local_screen_pt, local_screen_rtx_pt);
  } else {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, local_screen_pt);
  }
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0 ||
      append_fragment(out, out_cap, &off, bundled_transport, bundled_transport_len) != 0 ||
      append_line(out, out_cap, &off, is_audience ? "a=inactive" : "a=recvonly") != 0 || append_line(out, out_cap, &off, "a=mid:2") != 0 ||
      append_line(out, out_cap, &off, "a=rtcp-mux") != 0 || append_twcc_recv_attribute(out, out_cap, &off) != 0 ||
      append_mid_recv_attribute(out, out_cap, &off) != 0 ||
      append_video_codec_attributes(out, out_cap, &off, local_screen_codec, local_screen_pt, local_screen_rtx_pt) != 0) {
    goto fail;
  }

  for (uint32_t mid_audio = SFU_REMOTE_MID_BASE; mid_audio < next_remote_mid; mid_audio += SFU_REMOTE_TRANSCEIVERS_PER_SLOT) {
    uint32_t mid_video = mid_audio + 1;
    uint32_t mid_screen = mid_audio + 2;

    sfu_sdp_receiver_view_t slot;
    memset(&slot, 0, sizeof(slot));
    uint32_t map_slot = (mid_audio - SFU_REMOTE_MID_BASE) / SFU_REMOTE_TRANSCEIVERS_PER_SLOT;
    int16_t entry_index = map_slot < SFU_MAX_REMOTE_SLOTS ? mid_index[map_slot] : -1;
    bool found = entry_index >= 0 && sfu_sdp_receiver_view(snap, (uint32_t)entry_index, &slot);

    bool audio_live = found && slot.has_audio;
    bool video_live = found && slot.has_video;
    bool screen_live = found && slot.has_screen;
    if (append_bundled_audio(out, out_cap, &off, port, bundled_transport, bundled_transport_len, mid_audio, audio_live, found ? &slot : NULL) != 0) {
      goto fail;
    }
    if (append_bundled_video(out, out_cap, &off, port, bundled_transport, bundled_transport_len, mid_video, video_live, found ? &slot : NULL, local_video_pt,
                             local_rtx_pt, false) != 0 ||
        append_bundled_video(out, out_cap, &off, port, bundled_transport, bundled_transport_len, mid_screen, screen_live, found ? &slot : NULL, local_screen_pt,
                             local_screen_rtx_pt, true) != 0) {
      goto fail;
    }
  }

  if (snap) {
    sfu_subscriptions_snapshot_release(snap);
  }

  return (int)off;

fail:
  if (snap) {
    sfu_subscriptions_snapshot_release(snap);
  }
  return -1;
}
