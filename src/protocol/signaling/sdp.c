#include "protocol/signaling/sdp.h"
#include "util/log.h"

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
  // if (append_line(out, out_cap, offset, "a=ice-lite") != 0) {
  //   return -1;
  // }
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

static int append_video_codec_attributes(char *out, size_t out_cap, size_t *offset, uint8_t video_pt, uint8_t rtx_pt) {
  char line[128];
  int n;
  n = snprintf(line, sizeof(line), "a=rtpmap:%u VP8/90000", video_pt);
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

static int append_remote_audio_ssrcs(char *out, size_t out_cap, size_t *offset, uint32_t audio_ssrc, const char *ufrag) {
  char line[128];
  int n;
  if (audio_ssrc != 0) {
    n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", audio_ssrc);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    n = snprintf(line, sizeof(line), "a=ssrc:%u msid:%s remote-audio-track", audio_ssrc, ufrag);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    n = snprintf(line, sizeof(line), "a=msid:%s remote-audio-track", ufrag);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
  }
  return 0;
}

static int append_remote_video_ssrcs(char *out, size_t out_cap, size_t *offset, uint32_t video_ssrc, uint32_t rtx_ssrc, const char *ufrag) {
  char line[128];
  int n;
  if (video_ssrc != 0) {
    n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", video_ssrc);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    n = snprintf(line, sizeof(line), "a=ssrc:%u msid:%s remote-video-track", video_ssrc, ufrag);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    if (rtx_ssrc != 0) {
      n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", rtx_ssrc);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }
      n = snprintf(line, sizeof(line), "a=ssrc:%u msid:%s remote-video-track", rtx_ssrc, ufrag);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }
      n = snprintf(line, sizeof(line), "a=ssrc-group:FID %u %u", video_ssrc, rtx_ssrc);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }
    }
    n = snprintf(line, sizeof(line), "a=msid:%s remote-video-track", ufrag);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
  }
  return 0;
}


int sfu_sdp_build_initial_offer(const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, char *out, size_t out_cap) {
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

  if (append_line(out, out_cap, &off, "a=group:BUNDLE 0 1") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=ice-lite") != 0) {
    return -1;
  }

  const uint8_t video_pt = 96;
  const uint8_t rtx_pt = 97;

  n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);

  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    return -1;
  }

  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=recvonly") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=mid:0") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
    return -1;
  }

  n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, video_pt, rtx_pt);

  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    return -1;
  }

  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=recvonly") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=mid:1") != 0) {
    return -1;
  }

  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    return -1;
  }

  if (append_video_codec_attributes(out, out_cap, &off, video_pt, rtx_pt) != 0) {
    return -1;
  }

  return (int)off;
}

int sfu_sdp_build_answer(const sfu_peer_session_t *session, const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag,
                         const char *pwd, const char *fingerprint, char *out, size_t out_cap) {
  size_t off = 0;
  int in_media = 0;
  int saw_media_line = 0;
  int current_media = 0;

  uint8_t video_pt = session->uplink_video.payload_type ? session->uplink_video.payload_type : 96;
  uint8_t rtx_pt = session->uplink_video.rtx_payload_type ? session->uplink_video.rtx_payload_type : 97;

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
      for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
        const sfu_receiver_slot_t *slot = &session->receivers[i];

        if (!slot->audio && !slot->video) {
          continue;
        }

        if (slot->audio->ssrc != 0) {
          snprintf(bundle_line + strlen(bundle_line), sizeof(bundle_line) - strlen(bundle_line), " %u", tmp_mid++);
        }
        if (slot->video->ssrc != 0) {
          snprintf(bundle_line + strlen(bundle_line), sizeof(bundle_line) - strlen(bundle_line), " %u", tmp_mid++);
        }
      }
      if (append_line(out, out_cap, &off, bundle_line) != 0) {
        return -1;
      }
      continue;
    }

    if (starts_with(line, len, "o=")) {
      char o_line[128];
      int n = snprintf(o_line, sizeof(o_line), "o=- %ld 2 IN IP4 %s", (long)time(NULL), host);
      if (n < 0 || (size_t)n >= sizeof(o_line) || append_line_n(out, out_cap, &off, o_line, (size_t)n) != 0) {
        return -1;
      }
      continue;
    }

    if (starts_with(line, len, "a=sendonly")) {
      if (append_line(out, out_cap, &off, "a=recvonly") != 0) {
        return -1;
      }
      continue;
    }
    if (starts_with(line, len, "a=recvonly")) {
      if (append_line(out, out_cap, &off, "a=sendonly") != 0) {
        return -1;
      }
      continue;
    }
    if (starts_with(line, len, "a=sendrecv")) {
      if (append_line(out, out_cap, &off, "a=sendrecv") != 0) {
        return -1;
      }
      continue;
    }
    if (starts_with(line, len, "a=inactive")) {
      if (append_line(out, out_cap, &off, "a=inactive") != 0) {
        return -1;
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
        return -1;
      }
      const char *sp2 = memchr(sp1 + 1, ' ', len - (size_t)(sp1 + 1 - line));
      if (!sp2) {
        return -1;
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
        return -1;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        return -1;
      }
      if (current_media == 2 && video_pt != 0) {
        if (append_video_codec_attributes(out, out_cap, &off, video_pt, rtx_pt) != 0) {
          return -1;
        }
      }
      continue;
    }

    if (!in_media) {
      if (should_skip_line(line, len)) {
        continue;
      }
      if (append_line_n(out, out_cap, &off, line, len) != 0) {
        return -1;
      }
    } else {
      if (should_skip_line(line, len)) {
        continue;
      }
      if (current_media == 2 && video_pt != 0) {
        if (starts_with(line, len, "a=rtpmap:") || starts_with(line, len, "a=rtcp-fb:") || starts_with(line, len, "a=fmtp:")) {
          continue;
        }
      }
      if (append_line_n(out, out_cap, &off, line, len) != 0) {
        return -1;
      }
    }
  }

  if (!saw_media_line) {
    SFU_LOG_WARN("SDP offer had no m= line, cannot build an answer");
    return -1;
  }

  /* Generate dedicated media sections for all remote publishers at the end of the SDP */
  uint32_t mid_counter = 100;  // Offset MIDs to prevent clashes with client MIDs
  char buf[256];

  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    const sfu_receiver_slot_t *slot = &session->receivers[i];

    if (!slot->audio && !slot->video) {
      continue;
    }

    if (slot->audio->ssrc != 0) {
      snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
      if (append_line(out, out_cap, &off, buf) != 0) {
        return -1;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=sendonly") != 0) {
        return -1;
      }
      snprintf(buf, sizeof(buf), "a=mid:%u", mid_counter++);
      if (append_line(out, out_cap, &off, buf) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
        return -1;
      }
      if (append_remote_audio_ssrcs(out, out_cap, &off, slot->audio->ssrc, slot->audio->owner->ufrag) != 0) {
        return -1;
      }
    }

    if (slot->video->ssrc != 0) {
      if (video_pt != 0) {
        if (rtx_pt != 0) {
          snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, video_pt, rtx_pt);
        } else {
          snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, video_pt);
        }
      } else {
        snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF 96 97", port);  // Default Chrome fallback
      }
      if (append_line(out, out_cap, &off, buf) != 0) {
        return -1;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=sendonly") != 0) {
        return -1;
      }
      snprintf(buf, sizeof(buf), "a=mid:%u", mid_counter++);
      if (append_line(out, out_cap, &off, buf) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
        return -1;
      }

      if (video_pt != 0) {
        if (append_video_codec_attributes(out, out_cap, &off, video_pt, rtx_pt) != 0) {
          return -1;
        }
      } else {
        if (append_line(out, out_cap, &off, "a=rtpmap:96 VP8/90000") != 0) {
          return -1;
        }
        if (append_line(out, out_cap, &off, "a=rtcp-fb:96 nack") != 0) {
          return -1;
        }
        if (append_line(out, out_cap, &off, "a=rtcp-fb:96 nack pli") != 0) {
          return -1;
        }
        if (append_line(out, out_cap, &off, "a=rtpmap:97 rtx/90000") != 0) {
          return -1;
        }
        if (append_line(out, out_cap, &off, "a=fmtp:97 apt=96") != 0) {
          return -1;
        }
      }
      if (append_remote_video_ssrcs(out, out_cap, &off, slot->video->ssrc, slot->video->rtx_ssrc, slot->video->owner->ufrag) != 0) {
        return -1;
      }
    }
  }

  return (int)off;
}

int sfu_sdp_build_offer(const sfu_peer_session_t *session, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                        char *out, size_t out_cap) {
  size_t off = 0;
  char buf[512];
  int n;

  /* Session-level header. Fresh o= line since there is no inbound offer to mirror. */
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

  SFU_LOG_DEBUG("SDP_BUILD: receivers=%d", SFU_MAX_REMOTE_SLOTS);

  /* BUNDLE group up front: mid 0/1 are always the joining peer's own upload,
     the rest are one (audio) or two (audio+video) mids per remote publisher. */
  char bundle_line[512];
  size_t blen = (size_t)snprintf(bundle_line, sizeof(bundle_line), "a=group:BUNDLE 0 1");
  uint32_t remote_mid = 2;
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    const sfu_receiver_slot_t *slot = &session->receivers[i];

    if (!slot->audio && !slot->video) {
      continue;
    }

    const sfu_transceiver_t *audio = slot->audio;
    const sfu_transceiver_t *video = slot->video;

    if (audio->active && slot->audio->ssrc != 0) {
      n = snprintf(bundle_line + blen, sizeof(bundle_line) - blen, " %u", remote_mid++);
      if (n < 0 || (size_t)n >= sizeof(bundle_line) - blen) {
        return -1;
      }
      blen += (size_t)n;
    }

    if (video->active && video->ssrc != 0) {
      n = snprintf(bundle_line + blen, sizeof(bundle_line) - blen, " %u", remote_mid++);
      if (n < 0 || (size_t)n >= sizeof(bundle_line) - blen) {
        return -1;
      }
      blen += (size_t)n;
    }
  }
  if (append_line(out, out_cap, &off, bundle_line) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=ice-lite") != 0) {
    return -1;
  }

  uint8_t local_video_pt = session->uplink_video.payload_type ? session->uplink_video.payload_type : 96;
  uint8_t local_rtx_pt = session->uplink_video.rtx_payload_type ? session->uplink_video.rtx_payload_type : 97;

  /* mid 0: server RECEIVES the joining peer's own microphone. */
  n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    return -1;
  }
  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=recvonly") != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=mid:0") != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
    return -1;
  }

  /* mid 1: server RECEIVES the joining peer's own camera/screen. */
  if (local_rtx_pt != 0) {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, local_video_pt, local_rtx_pt);
  } else {
    n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, local_video_pt);
  }
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    return -1;
  }
  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=recvonly") != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=mid:1") != 0) {
    return -1;
  }
  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    return -1;
  }
  if (append_video_codec_attributes(out, out_cap, &off, local_video_pt, local_rtx_pt) != 0) {
    return -1;
  }

  /* mid 2..N: one SENDONLY section per remote publisher already in the room. */
  uint32_t mid_counter = 2;
  for (uint32_t i = 0; i < SFU_MAX_REMOTE_SLOTS; i++) {
    const sfu_receiver_slot_t *slot = &session->receivers[i];

    if (!slot->audio && !slot->video) {
      continue;
    }

    const sfu_transceiver_t *audio = slot->audio;
    const sfu_transceiver_t *video = slot->video;

    if (audio->active && audio->ssrc != 0) {
      n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
      if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
        return -1;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=sendonly") != 0) {
        return -1;
      }
      n = snprintf(buf, sizeof(buf), "a=mid:%u", mid_counter++);
      if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
        return -1;
      }
      if (append_remote_audio_ssrcs(out, out_cap, &off, audio->ssrc, audio->owner->ufrag) != 0) {
        return -1;
      }
    }

    if (video->active && video->ssrc != 0) {
      if (local_rtx_pt != 0) {
        n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, local_video_pt, local_rtx_pt);
      } else {
        n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, local_video_pt);
      }
      if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
        return -1;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=sendonly") != 0) {
        return -1;
      }
      n = snprintf(buf, sizeof(buf), "a=mid:%u", mid_counter++);
      if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
        return -1;
      }
      if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
        return -1;
      }
      if (append_video_codec_attributes(out, out_cap, &off, local_video_pt, local_rtx_pt) != 0) {
        return -1;
      }
      if (append_remote_video_ssrcs(out, out_cap, &off, video->ssrc, video->rtx_ssrc, video->owner->ufrag) != 0) {
        return -1;
      }
    }
  }

  return (int)off;
}
