#ifndef SFU_PROTOCOL_WEBSOCKET_H
#define SFU_PROTOCOL_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SFU_WS_PREFETCH_CAP 4096

typedef enum sfu_ws_frame_state { SFU_WS_STATE_HEADER = 0, SFU_WS_STATE_EXT_LEN, SFU_WS_STATE_MASK, SFU_WS_STATE_PAYLOAD } sfu_ws_frame_state_t;

typedef struct sfu_ws_read_state {
  uint8_t prefetched[SFU_WS_PREFETCH_CAP];
  size_t prefetched_offset;
  size_t prefetched_len;

  sfu_ws_frame_state_t frame_state;
  uint8_t hdr[2];
  size_t hdr_bytes;

  uint8_t ext_len_buf[8];
  size_t ext_len_needed;
  size_t ext_len_bytes;

  uint8_t mask_key[4];
  size_t mask_bytes;

  uint8_t opcode;
  int fin;
  int masked;
  uint64_t payload_len;
  uint64_t payload_read;

  uint8_t control_payload[125];

  char *msg_buf;
  size_t msg_cap;
  size_t msg_len;
  int fragmented;
} sfu_ws_read_state_t;

int sfu_ws_handshake(int fd, sfu_ws_read_state_t *state);

ssize_t sfu_ws_recv_text(int fd, sfu_ws_read_state_t *state, char *buf, size_t cap);

int sfu_ws_read_state_has_pending(const sfu_ws_read_state_t *state);

void sfu_ws_read_state_free(sfu_ws_read_state_t *state);

int sfu_ws_send_text(int fd, const char *data, size_t len);
int sfu_ws_send_close(int fd, uint16_t code, const char *reason, size_t reason_len);

#endif /* SFU_PROTOCOL_WEBSOCKET_H */
