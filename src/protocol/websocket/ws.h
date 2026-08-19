#ifndef SFU_PROTOCOL_WEBSOCKET_H
#define SFU_PROTOCOL_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SFU_WS_PREFETCH_CAP 4096

typedef struct sfu_ws_read_state {
  uint8_t prefetched[SFU_WS_PREFETCH_CAP];
  size_t prefetched_offset;
  size_t prefetched_len;
} sfu_ws_read_state_t;

int sfu_ws_handshake(int fd, sfu_ws_read_state_t *state);

ssize_t sfu_ws_recv_text(int fd, sfu_ws_read_state_t *state, char *buf, size_t cap);

int sfu_ws_read_state_has_pending(const sfu_ws_read_state_t *state);

int sfu_ws_send_text(int fd, const char *data, size_t len);

#endif /* SFU_PROTOCOL_WEBSOCKET_H */
