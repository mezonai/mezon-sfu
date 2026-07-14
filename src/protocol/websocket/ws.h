#ifndef SFU_PROTOCOL_WEBSOCKET_H
#define SFU_PROTOCOL_WEBSOCKET_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Minimal RFC 6455 WebSocket server, blocking, one OS thread per
 * connection. This is control-plane (signaling), not the hot media
 * path, so the lock-free/io_uring machinery elsewhere in this codebase
 * doesn't apply here -- a plain accept-loop with a thread per
 * connection is the right amount of complexity for a handful of
 * long-lived signaling connections.
 *
 * Only what mezon-sfu's signaling actually needs is implemented:
 * single-frame (non-fragmented) text messages, up to a 64-bit length
 * prefix (RFC 6455 6.1/6.2). Ping/pong is answered transparently
 * inside sfu_ws_recv_text's read loop; fragmented messages and binary
 * frames are not supported and will return an error.
 */

/* Performs the HTTP Upgrade handshake on an already-accepted TCP fd
 * (reads the request, computes Sec-WebSocket-Accept, writes the 101
 * response). Returns 0 on success, -1 on any parse/protocol error. */
int sfu_ws_handshake(int fd);

/* Blocks until one text frame arrives (transparently answering any
 * ping frames in between). Returns the payload length written into
 * buf (NUL-terminated within cap), 0 on a clean close frame, -1 on
 * error or an unsupported frame type. */
ssize_t sfu_ws_recv_text(int fd, char *buf, size_t cap);

/* Sends one unmasked text frame (RFC 6455: server-to-client frames
 * must NOT be masked). Returns 0 on success, -1 on write error. */
int sfu_ws_send_text(int fd, const char *data, size_t len);

#endif /* SFU_PROTOCOL_WEBSOCKET_H */
