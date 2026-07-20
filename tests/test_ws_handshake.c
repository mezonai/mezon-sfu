#include "protocol/websocket/ws.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  /* RFC 6455 section 1.3's own worked example: this exact key must
   * produce this exact accept value. If our SHA1+base64 handshake
   * logic is wrong, this is where it shows up. */
  {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    const char *request =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    assert(write(fds[1], request, strlen(request)) == (ssize_t)strlen(request));

    assert(sfu_ws_handshake(fds[0]) == 0);

    char response[512];
    ssize_t n = read(fds[1], response, sizeof(response) - 1);
    assert(n > 0);
    response[n] = '\0';

    assert(strstr(response, "101") != NULL);
    assert(strstr(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);

    close(fds[0]);
    close(fds[1]);
  }

  /* Frame round-trip: our send_text produces an unmasked frame a
   * client can decode, and our recv_text correctly unmasks a
   * client-style masked frame (RFC 6455 requires client->server
   * frames to be masked). */
  {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    const char *msg = "hello websocket";
    assert(sfu_ws_send_text(fds[0], msg, strlen(msg)) == 0);

    uint8_t raw[64];
    ssize_t n = read(fds[1], raw, sizeof(raw));
    assert(n == (ssize_t)(2 + strlen(msg))); /* 2-byte header, no mask, len < 126 */
    assert(raw[0] == 0x81);                  /* FIN=1, opcode=text */
    assert(raw[1] == (uint8_t)strlen(msg));  /* no mask bit */
    assert(memcmp(raw + 2, msg, strlen(msg)) == 0);

    /* Now the reverse: hand-construct a masked client frame and
     * confirm sfu_ws_recv_text unmasks it correctly. */
    const char *payload = "ping from client";
    size_t plen = strlen(payload);
    uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};

    uint8_t frame[128];
    size_t off = 0;
    frame[off++] = 0x81;                 /* FIN + text */
    frame[off++] = 0x80 | (uint8_t)plen; /* MASK bit + length */
    memcpy(frame + off, mask_key, 4);
    off += 4;
    for (size_t i = 0; i < plen; i++) {
      frame[off++] = (uint8_t)payload[i] ^ mask_key[i % 4];
    }

    assert(write(fds[1], frame, off) == (ssize_t)off);

    char buf[128];
    ssize_t rn = sfu_ws_recv_text(fds[0], buf, sizeof(buf));
    assert(rn == (ssize_t)plen);
    assert(memcmp(buf, payload, plen) == 0);

    close(fds[0]);
    close(fds[1]);
  }

  printf("test_ws_handshake: OK\n");
  return 0;
}
