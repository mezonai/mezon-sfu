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
#define WS_MSG_INITIAL_CAP 16384
#define WS_SEND_LOCK_COUNT 256

static pthread_mutex_t g_ws_send_locks[WS_SEND_LOCK_COUNT];
static pthread_once_t g_ws_send_locks_once = PTHREAD_ONCE_INIT;

static void init_ws_send_locks(void) {
  for (size_t i = 0; i < WS_SEND_LOCK_COUNT; i++) {
    pthread_mutex_init(&g_ws_send_locks[i], NULL);
  }
}

static int read_http_headers(int fd, uint8_t *buf, size_t cap, size_t *total_len, size_t *header_len) {
  size_t total = 0;
  while (total < cap) {
    ssize_t n = read(fd, buf + total, cap - total);
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
    for (size_t i = 3; i < total; i++) {
      if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') {
        *total_len = total;
        *header_len = i + 1;
        return 0;
      }
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

int sfu_ws_handshake(int fd, sfu_ws_read_state_t *state) {
  if (!state) {
    return -1;
  }
  memset(state, 0, sizeof(*state));

  uint8_t req_bytes[WS_HANDSHAKE_BUF_CAP + 1];
  size_t total_len = 0;
  size_t header_len = 0;
  if (read_http_headers(fd, req_bytes, WS_HANDSHAKE_BUF_CAP, &total_len, &header_len) != 0) {
    SFU_LOG_WARN("WS handshake: failed to read a complete HTTP request");
    return -1;
  }

  size_t trailing_len = total_len - header_len;
  if (trailing_len > sizeof(state->prefetched)) {
    SFU_LOG_WARN("WS handshake: prefetched frame data exceeds buffer capacity");
    return -1;
  }
  if (trailing_len > 0) {
    memcpy(state->prefetched, req_bytes + header_len, trailing_len);
    state->prefetched_len = trailing_len;
  }
  req_bytes[header_len] = '\0';
  const char *req = (const char *)req_bytes;

  char key[256];
  if (extract_header(req, "Sec-WebSocket-Key:", key, sizeof(key)) != 0 && extract_header(req, "sec-websocket-key:", key, sizeof(key)) != 0) {
    state->prefetched_len = 0;
    SFU_LOG_WARN("WS handshake: no Sec-WebSocket-Key header found");
    return -1;
  }

  char extensions[256];
  if (extract_header(req, "Sec-WebSocket-Extensions:", extensions, sizeof(extensions)) == 0 ||
      extract_header(req, "sec-websocket-extensions:", extensions, sizeof(extensions)) == 0) {
    SFU_LOG_INFO("WS handshake: declining Sec-WebSocket-Extensions (%s)", extensions);
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
    state->prefetched_len = 0;
    SFU_LOG_WARN("WS handshake: failed to write 101 response");
    return -1;
  }

  return 0;
}

int sfu_ws_read_state_has_pending(const sfu_ws_read_state_t *state) {
  if (!state) {
    return 0;
  }
  return state->prefetched_offset < state->prefetched_len;
}

static ssize_t read_bytes_nonblocking(int fd, sfu_ws_read_state_t *state, uint8_t *dst, size_t count) {
  size_t total = 0;
  if (state && state->prefetched_offset < state->prefetched_len) {
    size_t avail = state->prefetched_len - state->prefetched_offset;
    size_t take = avail < count ? avail : count;
    memcpy(dst, state->prefetched + state->prefetched_offset, take);
    state->prefetched_offset += take;
    total += take;
    if (state->prefetched_offset == state->prefetched_len) {
      state->prefetched_offset = 0;
      state->prefetched_len = 0;
    }
  }

  while (total < count) {
    ssize_t n = read(fd, dst + total, count - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (total > 0) {
          return (ssize_t)total;
        }
        return -1;
      }
      return -1;
    } else if (n == 0) {
      if (total > 0) {
        return (ssize_t)total;
      }
      return 0;
    }
    total += (size_t)n;
  }

  return (ssize_t)total;
}

static void reset_current_frame(sfu_ws_read_state_t *state) {
  state->frame_state = SFU_WS_STATE_HEADER;
  state->hdr_bytes = 0;
  state->ext_len_needed = 0;
  state->ext_len_bytes = 0;
  state->mask_bytes = 0;
  state->opcode = 0;
  state->fin = 0;
  state->masked = 0;
  state->payload_len = 0;
  state->payload_read = 0;
}

static void reset_all_state(sfu_ws_read_state_t *state) {
  reset_current_frame(state);
  state->msg_len = 0;
  state->fragmented = 0;
}

void sfu_ws_read_state_free(sfu_ws_read_state_t *state) {
  if (!state) {
    return;
  }
  reset_all_state(state);
  free(state->msg_buf);
  state->msg_buf = NULL;
  state->msg_cap = 0;
}

static int ensure_msg_capacity(sfu_ws_read_state_t *state, size_t needed) {
  if (needed <= state->msg_cap) {
    return 1;
  }
  size_t next = state->msg_cap ? state->msg_cap : WS_MSG_INITIAL_CAP;
  while (next < needed) {
    next *= 2;
  }
  char *grown = realloc(state->msg_buf, next);
  if (!grown) {
    return 0;
  }
  state->msg_buf = grown;
  state->msg_cap = next;
  return 1;
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

int sfu_ws_send_close(int fd, uint16_t code, const char *reason, size_t reason_len) {
  size_t payload_len = 2 + reason_len;
  if (payload_len > 125) {
    reason_len = 125 - 2;
    payload_len = 125;
  }
  uint8_t payload[125];
  payload[0] = (uint8_t)(code >> 8);
  payload[1] = (uint8_t)(code & 0xFF);
  if (reason_len > 0 && reason) {
    memcpy(payload + 2, reason, reason_len);
  }
  return send_frame(fd, 0x8, payload, payload_len);
}

ssize_t sfu_ws_recv_text(int fd, sfu_ws_read_state_t *state, char *buf, size_t cap) {
  if (!state || !buf || cap == 0) {
    errno = EINVAL;
    return -1;
  }
  errno = 0;

  for (;;) {
    if (state->frame_state == SFU_WS_STATE_HEADER) {
      while (state->hdr_bytes < 2) {
        ssize_t n = read_bytes_nonblocking(fd, state, state->hdr + state->hdr_bytes, 2 - state->hdr_bytes);
        if (n < 0) {
          return -1;
        }
        if (n == 0) {
          reset_all_state(state);
          return 0;
        }
        state->hdr_bytes += (size_t)n;
      }

      state->fin = (state->hdr[0] & 0x80) != 0;
      state->opcode = state->hdr[0] & 0x0F;
      state->masked = (state->hdr[1] & 0x80) != 0;
      uint64_t initial_len = state->hdr[1] & 0x7F;
      int control = (state->opcode & 0x08) != 0;

      if ((state->hdr[0] & 0x70) != 0) {
        SFU_LOG_WARN("WS: unsupported RSV bits (header=0x%02x opcode=%d), dropping connection", state->hdr[0], state->opcode);
        reset_all_state(state);
        errno = EPROTO;
        return -1;
      }

      if (control && (!state->fin || initial_len > 125)) {
        SFU_LOG_WARN("WS: invalid control frame, dropping connection");
        reset_all_state(state);
        errno = EPROTO;
        return -1;
      }

      if (!control) {
        if (state->opcode == 0x0) {
          if (!state->fragmented) {
            SFU_LOG_WARN("WS: continuation frame without fragmented text, dropping connection");
            reset_all_state(state);
            errno = EPROTO;
            return -1;
          }
        } else if (state->opcode == 0x1) {
          if (state->fragmented) {
            SFU_LOG_WARN("WS: new text frame during fragmented message, dropping connection");
            reset_all_state(state);
            errno = EPROTO;
            return -1;
          }
        } else {
          SFU_LOG_WARN("WS: unsupported data opcode %d, dropping connection", state->opcode);
          reset_all_state(state);
          errno = EPROTO;
          return -1;
        }
      }

      if (initial_len == 126) {
        state->ext_len_needed = 2;
        state->ext_len_bytes = 0;
        state->frame_state = SFU_WS_STATE_EXT_LEN;
      } else if (initial_len == 127) {
        state->ext_len_needed = 8;
        state->ext_len_bytes = 0;
        state->frame_state = SFU_WS_STATE_EXT_LEN;
      } else {
        state->payload_len = initial_len;
        state->payload_read = 0;
        if (state->masked) {
          state->mask_bytes = 0;
          state->frame_state = SFU_WS_STATE_MASK;
        } else {
          state->frame_state = SFU_WS_STATE_PAYLOAD;
        }
      }
    }

    if (state->frame_state == SFU_WS_STATE_EXT_LEN) {
      while (state->ext_len_bytes < state->ext_len_needed) {
        ssize_t n = read_bytes_nonblocking(fd, state, state->ext_len_buf + state->ext_len_bytes, state->ext_len_needed - state->ext_len_bytes);
        if (n < 0) {
          return -1;
        }
        if (n == 0) {
          reset_all_state(state);
          return 0;
        }
        state->ext_len_bytes += (size_t)n;
      }

      if (state->ext_len_needed == 2) {
        state->payload_len = ((uint64_t)state->ext_len_buf[0] << 8) | state->ext_len_buf[1];
      } else {
        if ((state->ext_len_buf[0] & 0x80) != 0) {
          SFU_LOG_WARN("WS: invalid 64-bit payload length, dropping connection");
          reset_all_state(state);
          errno = EPROTO;
          return -1;
        }
        state->payload_len = 0;
        for (int i = 0; i < 8; i++) {
          state->payload_len = (state->payload_len << 8) | state->ext_len_buf[i];
        }
      }

      state->payload_read = 0;
      if (state->masked) {
        state->mask_bytes = 0;
        state->frame_state = SFU_WS_STATE_MASK;
      } else {
        state->frame_state = SFU_WS_STATE_PAYLOAD;
      }
    }

    if (state->frame_state == SFU_WS_STATE_MASK) {
      while (state->mask_bytes < 4) {
        ssize_t n = read_bytes_nonblocking(fd, state, state->mask_key + state->mask_bytes, 4 - state->mask_bytes);
        if (n < 0) {
          return -1;
        }
        if (n == 0) {
          reset_all_state(state);
          return 0;
        }
        state->mask_bytes += (size_t)n;
      }
      state->frame_state = SFU_WS_STATE_PAYLOAD;
    }

    if (state->frame_state == SFU_WS_STATE_PAYLOAD) {
      int control = (state->opcode & 0x08) != 0;

      if (!control) {
        uint64_t remaining_in_frame = state->payload_len - state->payload_read;
        if (remaining_in_frame > (uint64_t)(cap - 1 - state->msg_len)) {
          SFU_LOG_WARN("WS: message payload exceeds buffer capacity (%zu)", cap);
          reset_all_state(state);
          errno = EPROTO;
          return -1;
        }
        size_t needed = state->msg_len + (size_t)remaining_in_frame + 1;
        if (!ensure_msg_capacity(state, needed)) {
          SFU_LOG_WARN("WS: message buffer allocation failed (%zu bytes)", needed);
          reset_all_state(state);
          errno = ENOMEM;
          return -1;
        }
      }

      if (control) {
        while (state->payload_read < state->payload_len) {
          size_t needed = (size_t)(state->payload_len - state->payload_read);
          ssize_t n = read_bytes_nonblocking(fd, state, state->control_payload + state->payload_read, needed);
          if (n < 0) {
            return -1;
          }
          if (n == 0) {
            reset_all_state(state);
            return 0;
          }
          if (state->masked) {
            for (size_t i = 0; i < (size_t)n; i++) {
              state->control_payload[state->payload_read + i] ^= state->mask_key[(state->payload_read + i) % 4];
            }
          }
          state->payload_read += (size_t)n;
        }

        uint8_t ctrl_op = state->opcode;
        size_t ctrl_len = (size_t)state->payload_len;
        reset_current_frame(state);

        switch (ctrl_op) {
          case 0x8:
            reset_all_state(state);
            return 0;
          case 0x9:
            if (send_frame(fd, 0xA, state->control_payload, ctrl_len) < 0) {
              reset_all_state(state);
              return -1;
            }
            continue;
          case 0xA:
            continue;
          default:
            SFU_LOG_WARN("WS: unsupported control opcode %d, dropping connection", ctrl_op);
            reset_all_state(state);
            errno = EPROTO;
            return -1;
        }
      } else {
        while (state->payload_read < state->payload_len) {
          size_t needed = (size_t)(state->payload_len - state->payload_read);
          uint8_t *dst = (uint8_t *)state->msg_buf + state->msg_len;
          ssize_t n = read_bytes_nonblocking(fd, state, dst, needed);
          if (n < 0) {
            return -1;
          }
          if (n == 0) {
            reset_all_state(state);
            return 0;
          }
          if (state->masked) {
            for (size_t i = 0; i < (size_t)n; i++) {
              dst[i] ^= state->mask_key[(state->payload_read + i) % 4];
            }
          }
          state->payload_read += (size_t)n;
          state->msg_len += (size_t)n;
        }

        int is_fin = state->fin;
        reset_current_frame(state);

        if (is_fin) {
          size_t final_len = state->msg_len;
          memcpy(buf, state->msg_buf, final_len);
          buf[final_len] = '\0';
          reset_all_state(state);
          return (ssize_t)final_len;
        } else {
          state->fragmented = 1;
        }
      }
    }
  }
}
