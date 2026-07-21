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

/* Appends `text` (length `len`) followed by "\r\n" to *out at *offset,
 * bounds-checked against out_cap. Returns 0 on success, -1 on overflow. */
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

/* Helper to append ICE, DTLS, and connection parameters to a media block */
static int append_media_transport_headers(char *out, size_t out_cap, size_t *offset, const char *host, uint16_t port, const char *ufrag, const char *pwd,
                                          const char *fingerprint) {
  char attr[512];
  int n;

  n = snprintf(attr, sizeof(attr), "c=IN IP4 %s", host);
  if (n < 0 || (size_t)n >= sizeof(attr) || append_line_n(out, out_cap, offset, attr, (size_t)n) != 0) {
    return -1;
  }

  if (append_line(out, out_cap, offset, "a=ice-lite") != 0) {
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

/* Appends the remote audio SSRCs directly into the client's audio m-line (mid:0)
 * Includes explicit SSRC-level msid mappings required by Firefox compatibility rules. */
static int append_remote_audio_ssrcs(char *out, size_t out_cap, size_t *offset, uint32_t audio_ssrc) {
  char line[128];
  int n;

  if (audio_ssrc != 0) {
    n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", audio_ssrc);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }

    /* FIX 1: Explicit SSRC-level msid mapping for Firefox */
    n = snprintf(line, sizeof(line), "a=ssrc:%u msid:remote-stream remote-audio-track", audio_ssrc);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }

    if (append_line(out, out_cap, offset, "a=msid:remote-stream remote-audio-track") != 0) {
      return -1;
    }
  }
  return 0;
}

/* Appends the remote video SSRCs directly into the client's video m-line (mid:1)
 * Includes explicit SSRC-level msid mappings required by Firefox compatibility rules. */
static int append_remote_video_ssrcs(char *out, size_t out_cap, size_t *offset, uint32_t video_ssrc, uint32_t rtx_ssrc) {
  char line[128];
  int n;

  /* Only generate video SSRC lines if a valid video SSRC is present[cite: 3] */
  if (video_ssrc != 0) {
    n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", video_ssrc);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }

    /* FIX 1: Explicit primary video SSRC-level msid mapping for Firefox */
    n = snprintf(line, sizeof(line), "a=ssrc:%u msid:remote-stream remote-video-track", video_ssrc);
    if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
      return -1;
    }

    /* Only generate the RTX SSRC line and FID grouping if an RTX SSRC was actually negotiated[cite: 3] */
    if (rtx_ssrc != 0) {
      n = snprintf(line, sizeof(line), "a=ssrc:%u cname:remote-peer", rtx_ssrc);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }

      /* FIX 1: Explicit RTX SSRC-level msid mapping for Firefox */
      n = snprintf(line, sizeof(line), "a=ssrc:%u msid:remote-stream remote-video-track", rtx_ssrc);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }

      n = snprintf(line, sizeof(line), "a=ssrc-group:FID %u %u", video_ssrc, rtx_ssrc);
      if (n < 0 || (size_t)n >= sizeof(line) || append_line_n(out, out_cap, offset, line, (size_t)n) != 0) {
        return -1;
      }
    }

    if (append_line(out, out_cap, offset, "a=msid:remote-stream remote-video-track") != 0) {
      return -1;
    }
  }

  return 0;
}

int sfu_sdp_build_answer(const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                         uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc, char *out, size_t out_cap) {
  size_t off = 0;
  int in_media = 0;  // 0 = parsing session level, 1 = parsing media level[cite: 3]
  int saw_media_line = 0;
  int current_media = 0;  // 0 = none, 1 = audio, 2 = video[cite: 3]

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

    // Keep the bundle group exactly as offered (e.g., "a=group:BUNDLE 0 1")[cite: 3]
    if (starts_with(line, len, "a=group:BUNDLE")) {
      if (append_line_n(out, out_cap, &off, line, len) != 0) {
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

    /* FIX 2: When a peer offers sendrecv (publishing media), answer with sendrecv.
     * Answering sendonly forces Firefox to shut down its outbound RTP sender! */
    if (starts_with(line, len, "a=sendrecv")) {
      if (append_line(out, out_cap, &off, "a=sendrecv") != 0) {
        return -1;
      }
      continue;
    }

    if (starts_with(line, len, "m=")) {
      // Before opening a new m-line, append the remote SSRCs for the completed section[cite: 3]
      if (current_media == 1) {
        if (append_remote_audio_ssrcs(out, out_cap, &off, audio_ssrc) != 0) {
          return -1;
        }
      } else if (current_media == 2) {
        if (append_remote_video_ssrcs(out, out_cap, &off, video_ssrc, rtx_ssrc) != 0) {
          return -1;
        }
      }

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
      int m_len = snprintf(m_line, sizeof(m_line), "%.*s %u%.*s", (int)(sp1 - line), line, port, (int)(len - (size_t)(sp2 - line)), sp2);
      if (m_len < 0 || (size_t)m_len >= sizeof(m_line) || append_line_n(out, out_cap, &off, m_line, (size_t)m_len) != 0) {
        return -1;
      }

      // Inject connection and transport configurations[cite: 3]
      if (append_media_transport_headers(out, out_cap, &off, host, port, ufrag, pwd, fingerprint) != 0) {
        return -1;
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
      if (append_line_n(out, out_cap, &off, line, len) != 0) {
        return -1;
      }
    }
  }

  if (!saw_media_line) {
    SFU_LOG_WARN("SDP offer had no m= line, cannot build an answer");
    return -1;
  }

  // Append remote SSRCs for the final media section at the end of parsing[cite: 3]
  if (current_media == 1) {
    if (append_remote_audio_ssrcs(out, out_cap, &off, audio_ssrc) != 0) {
      return -1;
    }
  } else if (current_media == 2) {
    if (append_remote_video_ssrcs(out, out_cap, &off, video_ssrc, rtx_ssrc) != 0) {
      return -1;
    }
  }

  return (int)off;
}
