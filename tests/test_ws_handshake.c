#include "protocol/websocket/ws.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void write_masked_frame(int fd, int fin, uint8_t opcode, const char *payload, size_t len, const uint8_t mask_key[4]) {
  assert(len < 126);
  uint8_t frame[256];
  size_t off = 0;
  frame[off++] = (fin ? 0x80 : 0) | opcode;
  frame[off++] = 0x80 | (uint8_t)len;
  memcpy(frame + off, mask_key, 4);
  off += 4;
  for (size_t i = 0; i < len; i++) {
    frame[off++] = (uint8_t)payload[i] ^ mask_key[i % 4];
  }
  assert(write(fd, frame, off) == (ssize_t)off);
}

static void open_pair(int fds[2]) { assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); }

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

  /* Fragmented text is reassembled across continuation frames. Each
   * fragment has its own mask, as required for client frames. */
  {
    int fds[2];
    open_pair(fds);
    const uint8_t mask1[4] = {1, 2, 3, 4};
    const uint8_t mask2[4] = {5, 6, 7, 8};
    const uint8_t mask3[4] = {9, 10, 11, 12};
    write_masked_frame(fds[1], 0, 0x1, "large ", 6, mask1);
    write_masked_frame(fds[1], 0, 0x0, "fragmented ", 11, mask2);
    write_masked_frame(fds[1], 1, 0x0, "message", 7, mask3);

    char buf[64];
    ssize_t n = sfu_ws_recv_text(fds[0], buf, sizeof(buf));
    assert(n == 24);
    assert(strcmp(buf, "large fragmented message") == 0);
    close(fds[0]);
    close(fds[1]);
  }

  /* Control frames may be interleaved without discarding the partial
   * message. A ping is answered with a pong carrying the same payload. */
  {
    int fds[2];
    open_pair(fds);
    const uint8_t mask[4] = {0x21, 0x43, 0x65, 0x87};
    write_masked_frame(fds[1], 0, 0x1, "hello ", 6, mask);
    write_masked_frame(fds[1], 1, 0x9, "alive", 5, mask);
    write_masked_frame(fds[1], 1, 0x0, "world", 5, mask);

    char buf[32];
    ssize_t n = sfu_ws_recv_text(fds[0], buf, sizeof(buf));
    assert(n == 11);
    assert(strcmp(buf, "hello world") == 0);

    uint8_t pong[16];
    assert(read(fds[1], pong, sizeof(pong)) == 7);
    assert(pong[0] == 0x8A);
    assert(pong[1] == 5);
    assert(memcmp(pong + 2, "alive", 5) == 0);
    close(fds[0]);
    close(fds[1]);
  }

  /* Invalid fragmentation sequences and aggregate overflow fail cleanly. */
  {
    int fds[2];
    open_pair(fds);
    const uint8_t mask[4] = {1, 1, 1, 1};
    write_masked_frame(fds[1], 1, 0x0, "orphan", 6, mask);
    char buf[32];
    assert(sfu_ws_recv_text(fds[0], buf, sizeof(buf)) == -1);
    close(fds[0]);
    close(fds[1]);
  }
  {
    int fds[2];
    open_pair(fds);
    const uint8_t mask[4] = {2, 2, 2, 2};
    write_masked_frame(fds[1], 0, 0x1, "first", 5, mask);
    write_masked_frame(fds[1], 1, 0x1, "second", 6, mask);
    char buf[32];
    assert(sfu_ws_recv_text(fds[0], buf, sizeof(buf)) == -1);
    close(fds[0]);
    close(fds[1]);
  }
  {
    int fds[2];
    open_pair(fds);
    const uint8_t mask[4] = {3, 3, 3, 3};
    write_masked_frame(fds[1], 0, 0x1, "12345", 5, mask);
    write_masked_frame(fds[1], 1, 0x0, "67890", 5, mask);
    char buf[10];
    assert(sfu_ws_recv_text(fds[0], buf, sizeof(buf)) == -1);
    close(fds[0]);
    close(fds[1]);
  }

  /* A close frame terminates reception even during fragmented assembly. */
  {
    int fds[2];
    open_pair(fds);
    const uint8_t mask[4] = {4, 4, 4, 4};
    write_masked_frame(fds[1], 0, 0x1, "partial", 7, mask);
    write_masked_frame(fds[1], 1, 0x8, "", 0, mask);
    char buf[32];
    assert(sfu_ws_recv_text(fds[0], buf, sizeof(buf)) == 0);
    close(fds[0]);
    close(fds[1]);
  }

  printf("test_ws_handshake: OK\n");
  return 0;
}
