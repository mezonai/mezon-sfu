#include "protocol/signaling/sdp.h"
#include "peer/session.h"
#include "util/log.h"

#include <assert.h>
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

#define SFU_TWCC_LOCAL_EXTMAP_ID 5

static int append_twcc_attributes(char *out, size_t out_cap, size_t *offset) {
  char line[160];
  int n = snprintf(line, sizeof(line), "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", SFU_TWCC_LOCAL_EXTMAP_ID);
  if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
    return -1;
  }
  return 0;
}

static int append_video_codec_attributes(char *out, size_t out_cap, size_t *offset, uint8_t video_pt, uint8_t rtx_pt) {
  sfu_video_codec_t codec = sfu_video_codec_from_pt(video_pt);
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

static int append_remote_audio_ssrcs(char *out, size_t out_cap, size_t *offset, uint32_t audio_ssrc, const char *ufrag) {
  char line[128];
  int n;
  if (audio_ssrc != 0) {
    n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", audio_ssrc);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    n = snprintf(line, sizeof(line), "a=ssrc:%u msid:%s audio-%s", audio_ssrc, ufrag, ufrag);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    n = snprintf(line, sizeof(line), "a=msid:%s audio-%s", ufrag, ufrag);
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
    n = snprintf(line, sizeof(line), "a=ssrc:%u msid:%s video-%s", video_ssrc, ufrag, ufrag);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
    if (rtx_ssrc != 0) {
      n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", rtx_ssrc);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }
      n = snprintf(line, sizeof(line), "a=ssrc:%u msid:%s video-%s", rtx_ssrc, ufrag, ufrag);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }
      n = snprintf(line, sizeof(line), "a=ssrc-group:FID %u %u", video_ssrc, rtx_ssrc);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }
    }
    n = snprintf(line, sizeof(line), "a=msid:%s video-%s", ufrag, ufrag);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }
  }
  return 0;
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

  if (append_line(out, out_cap, &off, "a=group:BUNDLE 0 1") != 0) {
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

  if (append_twcc_attributes(out, out_cap, &off) != 0) {
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

  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
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

  if (append_twcc_attributes(out, out_cap, &off) != 0) {
    return -1;
  }

  if (append_video_codec_attributes(out, out_cap, &off, SFU_PT_VP9, SFU_PT_VP9_RTX) != 0) {
    return -1;
  }
  if (append_video_codec_attributes(out, out_cap, &off, SFU_PT_AV1, SFU_PT_AV1_RTX) != 0) {
    return -1;
  }
  if (append_video_codec_attributes(out, out_cap, &off, SFU_PT_VP8, SFU_PT_VP8_RTX) != 0) {
    return -1;
  }

  return (int)off;
}

/* Fills a read-only view of snapshot entry `i`. Returns false when the entry
 * has no routing state. The caller must hold the snapshot. */
static bool sfu_sdp_receiver_view(const sfu_receiver_snapshot_t *snap, uint32_t i, sfu_sdp_receiver_view_t *view) {
  const sfu_receiver_entry_t *e = &snap->entries[i];
  if (!e->has_audio && !e->has_video) {
    return false;
  }
  view->audio_ssrc = e->audio_ssrc;
  view->video_ssrc = e->video_ssrc;
  view->video_rtx_ssrc = e->video_rtx_ssrc;
  view->mid_audio = e->mid_audio;
  view->mid_video = e->mid_video;
  view->video_pt = e->video_pt;
  view->video_rtx_pt = e->video_rtx_pt;
  view->has_audio = e->has_audio;
  view->has_video = e->has_video;
  view->audio_active = e->audio_active;
  view->video_active = e->video_active;
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
  assert(snap != NULL || receiver_count == 0);

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
          snprintf(bundle_line + strlen(bundle_line), sizeof(bundle_line) - strlen(bundle_line), " %u", tmp_mid++);
        }
        if (slot.has_video && slot.video_ssrc != 0) {
          snprintf(bundle_line + strlen(bundle_line), sizeof(bundle_line) - strlen(bundle_line), " %u", tmp_mid++);
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
        if (append_video_codec_attributes(out, out_cap, &off, video_pt, rtx_pt) != 0) {
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

  uint32_t mid_counter = 100;
  char buf[256];

  for (uint32_t i = 0; i < receiver_count; i++) {
    sfu_sdp_receiver_view_t slot;
    if (!sfu_sdp_receiver_view(snap, i, &slot)) {
      continue;
    }

    if (slot.has_audio && slot.audio_ssrc != 0) {
      snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
      if (append_line(out, out_cap, &off, buf) != 0) {
        goto fail;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        goto fail;
      }
      if (append_line(out, out_cap, &off, "a=sendonly") != 0) {
        goto fail;
      }
      snprintf(buf, sizeof(buf), "a=mid:%u", mid_counter++);
      if (append_line(out, out_cap, &off, buf) != 0) {
        goto fail;
      }
      if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
        goto fail;
      }
      if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
        goto fail;
      }
      if (append_remote_audio_ssrcs(out, out_cap, &off, slot.audio_ssrc, slot.owner_ufrag) != 0) {
        goto fail;
      }
    }

    if (slot.has_video && slot.video_ssrc != 0) {
      uint8_t remote_video_pt = slot.video_pt != 0 ? slot.video_pt : SFU_PT_VP8;
      uint8_t remote_rtx_pt = slot.video_rtx_pt != 0 ? slot.video_rtx_pt : SFU_PT_VP8_RTX;

      if (remote_rtx_pt != 0) {
        snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, remote_video_pt, remote_rtx_pt);
      } else {
        snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, remote_video_pt);
      }
      if (append_line(out, out_cap, &off, buf) != 0) {
        goto fail;
      }
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        goto fail;
      }
      if (append_line(out, out_cap, &off, "a=sendonly") != 0) {
        goto fail;
      }
      snprintf(buf, sizeof(buf), "a=mid:%u", mid_counter++);
      if (append_line(out, out_cap, &off, buf) != 0) {
        goto fail;
      }
      if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
        goto fail;
      }
      if (append_twcc_attributes(out, out_cap, &off) != 0) {
        goto fail;
      }
      if (append_video_codec_attributes(out, out_cap, &off, remote_video_pt, remote_rtx_pt) != 0) {
        goto fail;
      }
      if (append_remote_video_ssrcs(out, out_cap, &off, slot.video_ssrc, slot.video_rtx_ssrc, slot.owner_ufrag) != 0) {
        goto fail;
      }
    }
  }

  sfu_subscriptions_snapshot_release(snap);
  return (int)off;

fail:
  sfu_subscriptions_snapshot_release(snap);
  return -1;
}

int sfu_sdp_build_offer(const sfu_peer_session_t *session, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                        char *out, size_t out_cap) {
  size_t off = 0;
  char buf[512];
  int n;

  assert(session != NULL);

  sfu_receiver_snapshot_t *snap = sfu_session_subscriptions_acquire(session);
  uint32_t receiver_count = snap ? snap->count : 0;
  assert(snap != NULL || receiver_count == 0);

  /* Session-level header. Fresh o= line since there is no inbound offer to mirror. */
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

  SFU_LOG_DEBUG("SDP_BUILD: ufrag=%s receiver_count=%u", session->cold->ufrag, receiver_count);

  char bundle_line[512];
  size_t blen = (size_t)snprintf(bundle_line, sizeof(bundle_line), "a=group:BUNDLE 0 1");
  for (uint32_t mid_audio = 2; mid_audio < session->next_remote_mid; mid_audio += 2) {
    uint32_t mid_video = mid_audio + 1;
    n = snprintf(bundle_line + blen, sizeof(bundle_line) - blen, " %u %u", mid_audio, mid_video);
    if (n < 0 || (size_t)n >= sizeof(bundle_line) - blen) {
      goto fail;
    }
    blen += (size_t)n;
  }
  if (append_line(out, out_cap, &off, bundle_line) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=ice-lite") != 0) {
    goto fail;
  }

  uint8_t local_video_pt = session->uplink_video.payload_type ? session->uplink_video.payload_type : SFU_PT_VP8;
  uint8_t local_rtx_pt = session->uplink_video.rtx_payload_type ? session->uplink_video.rtx_payload_type : SFU_PT_VP8_RTX;

  n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
  if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
    goto fail;
  }
  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, atomic_load_explicit(&session->is_audience, memory_order_acquire) ? "a=inactive" : "a=recvonly") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=mid:0") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    goto fail;
  }
  if (append_twcc_attributes(out, out_cap, &off) != 0) {
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
  if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, atomic_load_explicit(&session->is_audience, memory_order_acquire) ? "a=inactive" : "a=recvonly") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=mid:1") != 0) {
    goto fail;
  }
  if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
    goto fail;
  }
  if (append_twcc_attributes(out, out_cap, &off) != 0) {
    goto fail;
  }
  if (append_video_codec_attributes(out, out_cap, &off, local_video_pt, local_rtx_pt) != 0) {
    goto fail;
  }

  for (uint32_t mid_audio = 2; mid_audio < session->next_remote_mid; mid_audio += 2) {
    uint32_t mid_video = mid_audio + 1;

    sfu_sdp_receiver_view_t slot;
    bool found = false;
    for (uint32_t i = 0; i < receiver_count; i++) {
      sfu_sdp_receiver_view_t candidate;
      if (!sfu_sdp_receiver_view(snap, i, &candidate)) {
        continue;
      }
      if (candidate.mid_audio == mid_audio) {
        slot = candidate;
        found = true;
        break;
      }
    }

    bool audio_live = found && slot.has_audio && slot.audio_active && slot.audio_ssrc != 0;
    bool video_live = found && slot.has_video && slot.video_active && slot.video_ssrc != 0;

    n = snprintf(buf, sizeof(buf), "m=audio %u UDP/TLS/RTP/SAVPF 111", port);
    if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
      goto fail;
    }
    if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
      goto fail;
    }
    if (append_line(out, out_cap, &off, audio_live ? "a=sendonly" : "a=inactive") != 0) {
      goto fail;
    }
    n = snprintf(buf, sizeof(buf), "a=mid:%u", mid_audio);
    if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
      goto fail;
    }
    if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
      goto fail;
    }
    if (append_line(out, out_cap, &off, "a=rtpmap:111 opus/48000/2") != 0) {
      goto fail;
    }
    if (audio_live) {
      if (append_remote_audio_ssrcs(out, out_cap, &off, slot.audio_ssrc, slot.owner_ufrag) != 0) {
        goto fail;
      }
    }

    uint8_t remote_video_pt = (video_live && slot.video_pt != 0) ? slot.video_pt : local_video_pt;
    uint8_t remote_rtx_pt = (video_live && slot.video_rtx_pt != 0) ? slot.video_rtx_pt : local_rtx_pt;

    if (remote_rtx_pt != 0) {
      n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u %u", port, remote_video_pt, remote_rtx_pt);
    } else {
      n = snprintf(buf, sizeof(buf), "m=video %u UDP/TLS/RTP/SAVPF %u", port, remote_video_pt);
    }
    if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
      goto fail;
    }
    if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
      goto fail;
    }
    if (append_line(out, out_cap, &off, video_live ? "a=sendonly" : "a=inactive") != 0) {
      goto fail;
    }
    n = snprintf(buf, sizeof(buf), "a=mid:%u", mid_video);
    if (n < 0 || (size_t)n >= sizeof(buf) || append_line_n(out, out_cap, &off, buf, (size_t)n) != 0) {
      goto fail;
    }
    if (append_line(out, out_cap, &off, "a=rtcp-mux") != 0) {
      goto fail;
    }
    if (append_twcc_attributes(out, out_cap, &off) != 0) {
      goto fail;
    }
    if (append_video_codec_attributes(out, out_cap, &off, remote_video_pt, remote_rtx_pt) != 0) {
      goto fail;
    }
    if (video_live) {
      if (append_remote_video_ssrcs(out, out_cap, &off, slot.video_ssrc, slot.video_rtx_ssrc, slot.owner_ufrag) != 0) {
        goto fail;
      }
    }
  }

  sfu_subscriptions_snapshot_release(snap);

  return (int)off;

fail:
  sfu_subscriptions_snapshot_release(snap);
  return -1;
}
