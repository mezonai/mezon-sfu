#include "protocol/websocket/ws.h"
#include "util/log.h"

#include <ctype.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_HANDSHAKE_BUF_CAP 4096

static ssize_t read_full_available(int fd, char *buf, size_t cap,
                                   const char *terminator) {
  size_t total = 0;
  while (total < cap - 1) {
    ssize_t n = read(fd, buf + total, cap - 1 - total);
    if (n <= 0)
      return -1;
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, terminator))
      return (ssize_t)total;
  }
  return -1; /* request too large, not a real HTTP upgrade */
}

/* Case-insensitive header value extraction from a raw HTTP request
 * buffer. Returns 0 and fills out (NUL-terminated) on success. */
static int extract_header(const char *req, const char *header_name, char *out,
                          size_t out_cap) {
  size_t name_len = strlen(header_name);
  const char *p = req;
  while ((p = strstr(p, header_name)) != NULL) {
    /* Ensure this match starts at a line boundary (not a substring
     * of a longer header name). */
    if (p != req && p[-1] != '\n') {
      p += name_len;
      continue;
    }
    const char *value_start = p + name_len;
    while (*value_start == ' ')
      value_start++;
    const char *line_end = strstr(value_start, "\r\n");
    if (!line_end)
      return -1;
    size_t len = (size_t)(line_end - value_start);
    if (len >= out_cap)
      return -1;
    memcpy(out, value_start, len);
    out[len] = '\0';
    return 0;
  }
  return -1;
}

static void base64_encode(const uint8_t *data, int len, char *out) {
  EVP_EncodeBlock((unsigned char *)out, data, len);
}

int sfu_ws_handshake(int fd) {
  char req[WS_HANDSHAKE_BUF_CAP];
  if (read_full_available(fd, req, sizeof(req), "\r\n\r\n") < 0) {
    SFU_LOG_WARN("WS handshake: failed to read a complete HTTP request");
    return -1;
  }

  char key[256];
  /* Case-insensitive search: browsers send "Sec-WebSocket-Key" but be
   * lenient since HTTP header names are case-insensitive by spec. */
  if (extract_header(req, "Sec-WebSocket-Key:", key, sizeof(key)) != 0 &&
      extract_header(req, "sec-websocket-key:", key, sizeof(key)) != 0) {
    SFU_LOG_WARN("WS handshake: no Sec-WebSocket-Key header found");
    return -1;
  }

  char concat[256 + sizeof(WS_GUID)];
  int concat_len = snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);

  unsigned char digest[SHA_DIGEST_LENGTH];
  unsigned int digest_len = 0;
  EVP_Digest(concat, (size_t)concat_len, digest, &digest_len, EVP_sha1(), NULL);

  char accept[64];
  base64_encode(digest, (int)digest_len, accept);

  char response[512];
  int resp_len = snprintf(response, sizeof(response),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n"
                          "\r\n",
                          accept);

  if (write(fd, response, (size_t)resp_len) != resp_len) {
    SFU_LOG_WARN("WS handshake: failed to write 101 response");
    return -1;
  }

  return 0;
}

static ssize_t read_exact(int fd, uint8_t *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(fd, buf + total, len - total);
    if (n <= 0)
      return -1;
    total += (size_t)n;
  }
  return (ssize_t)total;
}

static int send_frame(int fd, uint8_t opcode, const uint8_t *payload,
                      size_t len) {
  uint8_t header[10];
  size_t header_len = 0;
  header[0] = 0x80 | opcode; /* FIN=1 */

  if (len < 126) {
    header[1] = (uint8_t)len; /* MASK bit unset: server frames unmasked */
    header_len = 2;
  } else if (len <= 0xFFFF) {
    header[1] = 126;
    header[2] = (uint8_t)(len >> 8);
    header[3] = (uint8_t)(len & 0xFF);
    header_len = 4;
  } else {
    header[1] = 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (uint8_t)(len >> (8 * (7 - i)));
    }
    header_len = 10;
  }

  if (write(fd, header, header_len) != (ssize_t)header_len)
    return -1;
  if (len > 0 && write(fd, payload, len) != (ssize_t)len)
    return -1;
  return 0;
}

int sfu_ws_send_text(int fd, const char *data, size_t len) {
  return send_frame(fd, 0x1, (const uint8_t *)data, len);
}

ssize_t sfu_ws_recv_text(int fd, char *buf, size_t cap) {
  for (;;) {
    uint8_t header[2];
    if (read_exact(fd, header, 2) < 0)
      return -1;

    int fin = (header[0] & 0x80) != 0;
    int opcode = header[0] & 0x0F;
    int masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (!fin) {
      SFU_LOG_WARN("WS: fragmented frames not supported, dropping connection");
      return -1;
    }

    if (payload_len == 126) {
      uint8_t ext[2];
      if (read_exact(fd, ext, 2) < 0)
        return -1;
      payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
      uint8_t ext[8];
      if (read_exact(fd, ext, 8) < 0)
        return -1;
      payload_len = 0;
      for (int i = 0; i < 8; i++)
        payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
      if (read_exact(fd, mask_key, 4) < 0)
        return -1;
    }

    if (payload_len >= cap) {
      SFU_LOG_WARN("WS: frame payload (%llu) exceeds buffer capacity (%zu)",
                   (unsigned long long)payload_len, cap);
      return -1;
    }

    if (payload_len > 0 && read_exact(fd, (uint8_t *)buf, payload_len) < 0)
      return -1;
    if (masked) {
      for (uint64_t i = 0; i < payload_len; i++) {
        ((uint8_t *)buf)[i] ^= mask_key[i % 4];
      }
    }
    buf[payload_len] = '\0';

    switch (opcode) {
    case 0x1: /* text */
      return (ssize_t)payload_len;
    case 0x8: /* close */
      return 0;
    case 0x9: /* ping -> answer with pong, keep looping for the real message */
      send_frame(fd, 0xA, (const uint8_t *)buf, payload_len);
      continue;
    case 0xA: /* pong: nothing to do */
      continue;
    default:
      SFU_LOG_WARN("WS: unsupported opcode %d, dropping connection", opcode);
      return -1;
    }
  }
}
