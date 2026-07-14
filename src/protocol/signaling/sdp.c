#include "protocol/signaling/sdp.h"
#include "util/log.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static int starts_with(const char *s, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    return len >= plen && memcmp(s, prefix, plen) == 0;
}

static const char *SKIP_PREFIXES[] = {
    "a=ice-ufrag", "a=ice-pwd", "a=fingerprint", "a=setup",
    "a=candidate", "c=IN", "a=ice-options", "a=end-of-candidates", NULL
};

static int should_skip_in_media(const char *line, size_t len) {
    for (int i = 0; SKIP_PREFIXES[i]; i++) {
        if (starts_with(line, len, SKIP_PREFIXES[i])) return 1;
    }
    return 0;
}

/* Appends `text` (length `len`) followed by "\r\n" to *out at *offset,
 * bounds-checked against out_cap. Returns 0 on success, -1 on overflow. */
static int append_line_n(char *out, size_t out_cap, size_t *offset, const char *text, size_t len) {
    if (*offset + len + 2 >= out_cap) return -1;
    memcpy(out + *offset, text, len);
    *offset += len;
    out[(*offset)++] = '\r';
    out[(*offset)++] = '\n';
    return 0;
}

static int append_line(char *out, size_t out_cap, size_t *offset, const char *text) {
    return append_line_n(out, out_cap, offset, text, strlen(text));
}

int sfu_sdp_build_answer(const char *offer, size_t offer_len,
                          const char *host, uint16_t port,
                          const char *ufrag, const char *pwd,
                          const char *fingerprint,
                          char *out, size_t out_cap) {
    size_t off = 0;
    int in_media = 0;
    int saw_media_line = 0;

    size_t pos = 0;
    while (pos < offer_len) {
        size_t line_start = pos;
        while (pos < offer_len && offer[pos] != '\n') pos++;
        size_t line_end = pos; /* exclusive, points at '\n' or offer_len */
        if (line_end > line_start && offer[line_end - 1] == '\r') line_end--;
        if (pos < offer_len) pos++; /* consume '\n' */

        const char *line = offer + line_start;
        size_t len = line_end - line_start;
        if (len == 0) continue;

        if (starts_with(line, len, "m=")) {
            saw_media_line = 1;
            in_media = 1;

            /* Replace the port token (2nd space-separated field) with
             * our media port; keep media type + everything after the
             * port (proto, payload type list) unchanged. */
            const char *sp1 = memchr(line, ' ', len);
            if (!sp1) return -1;
            const char *sp2 = memchr(sp1 + 1, ' ', len - (size_t)(sp1 + 1 - line));
            if (!sp2) return -1;

            char m_line[256];
            int m_len = snprintf(m_line, sizeof(m_line), "%.*s %u%.*s",
                                  (int)(sp1 - line), line, port,
                                  (int)(len - (size_t)(sp2 - line)), sp2);
            if (m_len < 0 || append_line_n(out, out_cap, &off, m_line, (size_t)m_len) != 0) return -1;

            char attr[512];
            int n;
            n = snprintf(attr, sizeof(attr), "c=IN IP4 %s", host);
            if (append_line_n(out, out_cap, &off, attr, (size_t)n) != 0) return -1;
            if (append_line(out, out_cap, &off, "a=ice-lite") != 0) return -1;
            n = snprintf(attr, sizeof(attr), "a=ice-ufrag:%s", ufrag);
            if (append_line_n(out, out_cap, &off, attr, (size_t)n) != 0) return -1;
            n = snprintf(attr, sizeof(attr), "a=ice-pwd:%s", pwd);
            if (append_line_n(out, out_cap, &off, attr, (size_t)n) != 0) return -1;
            n = snprintf(attr, sizeof(attr), "a=fingerprint:sha-256 %s", fingerprint);
            if (append_line_n(out, out_cap, &off, attr, (size_t)n) != 0) return -1;
            if (append_line(out, out_cap, &off, "a=setup:passive") != 0) return -1;
            n = snprintf(attr, sizeof(attr), "a=candidate:1 1 udp 2130706431 %s %u typ host", host, port);
            if (append_line_n(out, out_cap, &off, attr, (size_t)n) != 0) return -1;
            if (append_line(out, out_cap, &off, "a=end-of-candidates") != 0) return -1;
            continue;
        }

        if (starts_with(line, len, "o=")) {
            char o_line[128];
            int n = snprintf(o_line, sizeof(o_line), "o=- %ld 2 IN IP4 %s", (long)time(NULL), host);
            if (append_line_n(out, out_cap, &off, o_line, (size_t)n) != 0) return -1;
            continue;
        }

        if (!in_media) {
            if (append_line_n(out, out_cap, &off, line, len) != 0) return -1;
            continue;
        }

        if (should_skip_in_media(line, len)) continue;
        if (append_line_n(out, out_cap, &off, line, len) != 0) return -1;
    }

    if (!saw_media_line) {
        SFU_LOG_WARN("SDP offer had no m= line, cannot build an answer");
        return -1;
    }

    return (int)off;
}
