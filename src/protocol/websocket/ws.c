#include "protocol/websocket/ws.h"
#include "util/log.h"

#include <errno.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_HANDSHAKE_BUF_CAP 4096
#define WS_SEND_LOCK_COUNT 256

static pthread_mutex_t g_ws_send_locks[WS_SEND_LOCK_COUNT];
static pthread_once_t g_ws_send_locks_once = PTHREAD_ONCE_INIT;

static void init_ws_send_locks(void) {
  for (size_t i = 0; i < WS_SEND_LOCK_COUNT; i++) {
    pthread_mutex_init(&g_ws_send_locks[i], NULL);
  }
}

static ssize_t read_full_available(int fd, char *buf, size_t cap, const char *terminator) {
  size_t total = 0;
  while (total < cap - 1) {
    ssize_t n = read(fd, buf + total, cap - 1 - total);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        if (total > 0) {
          struct pollfd pfd = {.fd = fd, .events = POLLIN};
          if (poll(&pfd, 1, 100) > 0) {
            continue;
          }
        }
        return -1;
      }
      return -1;
    } else if (n == 0) {
      return -1;
    }
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, terminator)) {
      return (ssize_t)total;
    }
  }
  return -1;
}

static int extract_header(const char *req, const char *header_name, char *out, size_t out_cap) {
  size_t name_len = strlen(header_name);
  const char *p = req;
  while ((p = strstr(p, header_name)) != NULL) {
    if (p != req && p[-1] != '\n') {
      p += name_len;
      continue;
    }
    const char *value_start = p + name_len;
    while (*value_start == ' ') {
      value_start++;
    }
    const char *line_end = strstr(value_start, "\r\n");
    if (!line_end) {
      return -1;
    }
    size_t len = (size_t)(line_end - value_start);
    if (len >= out_cap) {
      return -1;
    }
    memcpy(out, value_start, len);
    out[len] = '\0';
    return 0;
  }
  return -1;
}

static void base64_encode(const uint8_t *data, int len, char *out) { EVP_EncodeBlock((unsigned char *)out, data, len); }

static int write_exact(int fd, const uint8_t *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = write(fd, buf + total, len - total);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        if (poll(&pfd, 1, 100) > 0) {
          continue;
        }
      }
      return -1;
    } else if (n == 0) {
      return -1;
    }
    total += (size_t)n;
  }
  return 0;
}

int sfu_ws_handshake(int fd) {
  char req[WS_HANDSHAKE_BUF_CAP];
  if (read_full_available(fd, req, sizeof(req), "\r\n\r\n") < 0) {
    SFU_LOG_WARN("WS handshake: failed to read a complete HTTP request");
    return -1;
  }

  char key[256];
  if (extract_header(req, "Sec-WebSocket-Key:", key, sizeof(key)) != 0 && extract_header(req, "sec-websocket-key:", key, sizeof(key)) != 0) {
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

  if (write_exact(fd, (const uint8_t *)response, (size_t)resp_len) < 0) {
    SFU_LOG_WARN("WS handshake: failed to write 101 response");
    return -1;
  }

  return 0;
}

static ssize_t read_exact(int fd, uint8_t *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(fd, buf + total, len - total);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int res = poll(&pfd, 1, 100);
        if (res <= 0) {
          return -1;
        }
        continue;
      }
      return -1;
    } else if (n == 0) {
      return -1;
    }
    total += (size_t)n;
  }
  return (ssize_t)total;
}

static int send_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t len) {
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

  pthread_once(&g_ws_send_locks_once, init_ws_send_locks);
  pthread_mutex_t *send_lock = &g_ws_send_locks[(unsigned)fd % WS_SEND_LOCK_COUNT];
  pthread_mutex_lock(send_lock);
  int result = 0;
  if (write_exact(fd, header, header_len) < 0 || (len > 0 && write_exact(fd, payload, len) < 0)) {
    result = -1;
  }
  pthread_mutex_unlock(send_lock);
  return result;
}

int sfu_ws_send_text(int fd, const char *data, size_t len) { return send_frame(fd, 0x1, (const uint8_t *)data, len); }

ssize_t sfu_ws_recv_text(int fd, char *buf, size_t cap) {
  if (!buf || cap == 0) {
    return -1;
  }

  size_t message_len = 0;
  int fragmented_text = 0;

  for (;;) {
    uint8_t header[2];
    if (read_exact(fd, header, 2) < 0) {
      return -1;
    }

    int fin = (header[0] & 0x80) != 0;
    int opcode = header[0] & 0x0F;
    int masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;
    int control = (opcode & 0x08) != 0;

    if ((header[0] & 0x70) != 0) {
      SFU_LOG_WARN("WS: unsupported RSV bits, dropping connection");
      return -1;
    }

    if (payload_len == 126) {
      uint8_t ext[2];
      if (read_exact(fd, ext, 2) < 0) {
        return -1;
      }
      payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
      uint8_t ext[8];
      if (read_exact(fd, ext, 8) < 0) {
        return -1;
      }
      payload_len = 0;
      for (int i = 0; i < 8; i++) {
        payload_len = (payload_len << 8) | ext[i];
      }
      if ((ext[0] & 0x80) != 0) {
        SFU_LOG_WARN("WS: invalid 64-bit payload length, dropping connection");
        return -1;
      }
    }

    if (control && (!fin || payload_len > 125)) {
      SFU_LOG_WARN("WS: invalid control frame, dropping connection");
      return -1;
    }

    uint8_t mask_key[4] = {0};
    if (masked && read_exact(fd, mask_key, 4) < 0) {
      return -1;
    }

    if (control) {
      uint8_t control_payload[125];
      if (payload_len > 0 && read_exact(fd, control_payload, (size_t)payload_len) < 0) {
        return -1;
      }
      if (masked) {
        for (size_t i = 0; i < (size_t)payload_len; i++) {
          control_payload[i] ^= mask_key[i % 4];
        }
      }

      switch (opcode) {
        case 0x8: /* close */
          return 0;
        case 0x9: /* ping -> answer with pong, preserve any partial text message */
          if (send_frame(fd, 0xA, control_payload, (size_t)payload_len) < 0) {
            return -1;
          }
          continue;
        case 0xA: /* pong */
          continue;
        default:
          SFU_LOG_WARN("WS: unsupported control opcode %d, dropping connection", opcode);
          return -1;
      }
    }

    if (opcode == 0x0) {
      if (!fragmented_text) {
        SFU_LOG_WARN("WS: continuation frame without fragmented text, dropping connection");
        return -1;
      }
    } else if (opcode == 0x1) {
      if (fragmented_text) {
        SFU_LOG_WARN("WS: new text frame during fragmented message, dropping connection");
        return -1;
      }
    } else {
      SFU_LOG_WARN("WS: unsupported data opcode %d, dropping connection", opcode);
      return -1;
    }

    if (payload_len > (uint64_t)(cap - 1 - message_len)) {
      SFU_LOG_WARN("WS: message payload exceeds buffer capacity (%zu)", cap);
      return -1;
    }

    size_t fragment_len = (size_t)payload_len;
    if (fragment_len > 0 && read_exact(fd, (uint8_t *)buf + message_len, fragment_len) < 0) {
      return -1;
    }
    if (masked) {
      for (size_t i = 0; i < fragment_len; i++) {
        ((uint8_t *)buf)[message_len + i] ^= mask_key[i % 4];
      }
    }
    message_len += fragment_len;
    buf[message_len] = '\0';

    if (fin) {
      return (ssize_t)message_len;
    }
    fragmented_text = 1;
  }
}
