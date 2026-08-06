#ifndef SFU_PROTOCOL_WEBSOCKET_H
#define SFU_PROTOCOL_WEBSOCKET_H

#include <stddef.h>
#include <sys/types.h>

int sfu_ws_handshake(int fd);

ssize_t sfu_ws_recv_text(int fd, char *buf, size_t cap);

int sfu_ws_send_text(int fd, const char *data, size_t len);

#endif /* SFU_PROTOCOL_WEBSOCKET_H */
