#include "protocol/websocket/ws.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static size_t build_masked_frame(uint8_t *frame, int fin, uint8_t opcode, const char *payload, size_t len, const uint8_t mask_key[4]) {
  size_t off = 0;
  frame[off++] = (fin ? 0x80 : 0) | opcode;
  if (len < 126) {
    frame[off++] = 0x80 | (uint8_t)len;
  } else if (len <= 0xFFFF) {
    frame[off++] = 0x80 | 126;
    frame[off++] = (uint8_t)(len >> 8);
    frame[off++] = (uint8_t)(len & 0xFF);
  } else {
    frame[off++] = 0x80 | 127;
    for (int i = 0; i < 8; i++) {
      frame[off++] = (uint8_t)(len >> (8 * (7 - i)));
    }
  }
  if (mask_key) {
    memcpy(frame + off, mask_key, 4);
    off += 4;
    for (size_t i = 0; i < len; i++) {
      frame[off++] = (uint8_t)payload[i] ^ mask_key[i % 4];
    }
  } else {
    frame[1] &= 0x7F;
    for (size_t i = 0; i < len; i++) {
      frame[off++] = (uint8_t)payload[i];
    }
  }
  return off;
}

static void write_masked_frame(int fd, int fin, uint8_t opcode, const char *payload, size_t len, const uint8_t mask_key[4]) {
  uint8_t frame[256];
  size_t frame_len = build_masked_frame(frame, fin, opcode, payload, len, mask_key);
  assert(write(fd, frame, frame_len) == (ssize_t)frame_len);
}

static void open_pair(int fds[2]) { assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0); }

int main(void) {
  sfu_ws_read_state_t state = {0};
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

    assert(sfu_ws_handshake(fds[0], &state) == 0);

    char response[512];
    ssize_t n = read(fds[1], response, sizeof(response) - 1);
    assert(n > 0);
    response[n] = '\0';

    assert(strstr(response, "101") != NULL);
    assert(strstr(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);

    close(fds[0]);
    close(fds[1]);
  }

  /* permessage-deflate must not be accepted: RSV1 later kills the socket. */
  {
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    const char *request =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n\r\n";
    assert(write(fds[1], request, strlen(request)) == (ssize_t)strlen(request));

    assert(sfu_ws_handshake(fds[0], &state) == 0);

    char response[512];
    ssize_t n = read(fds[1], response, sizeof(response) - 1);
    assert(n > 0);
    response[n] = '\0';

    assert(strstr(response, "101") != NULL);
    assert(strstr(response, "Sec-WebSocket-Extensions") == NULL);

    close(fds[0]);
    close(fds[1]);
  }

  /* Upgrade headers and the first frame may be coalesced into one TCP read.
   * Handshake must preserve every byte after the HTTP terminator. */
  {
    int fds[2];
    open_pair(fds);
    const char *request =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    const char *payload = "first message";
    const uint8_t mask[4] = {0x10, 0x20, 0x30, 0x40};
    uint8_t frame[64];
    size_t frame_len = build_masked_frame(frame, 1, 0x1, payload, strlen(payload), mask);
    uint8_t combined[512];
    size_t request_len = strlen(request);
    memcpy(combined, request, request_len);
    memcpy(combined + request_len, frame, frame_len);
    assert(write(fds[1], combined, request_len + frame_len) == (ssize_t)(request_len + frame_len));

    assert(sfu_ws_handshake(fds[0], &state) == 0);
    assert(sfu_ws_read_state_has_pending(&state));
    char buf[64];
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == (ssize_t)strlen(payload));
    assert(strcmp(buf, payload) == 0);
    assert(!sfu_ws_read_state_has_pending(&state));
    close(fds[0]);
    close(fds[1]);
  }

  /* Prefetched frame bytes combine with later socket bytes, even when the
   * handshake read ends in the middle of the mask key. */
  {
    int fds[2];
    open_pair(fds);
    const char *request =
        "GET /chat HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    const char *payload = "split frame";
    const uint8_t mask[4] = {1, 3, 5, 7};
    uint8_t frame[64];
    size_t frame_len = build_masked_frame(frame, 1, 0x1, payload, strlen(payload), mask);
    size_t request_len = strlen(request);
    uint8_t combined[512];
    memcpy(combined, request, request_len);
    memcpy(combined + request_len, frame, 4);
    assert(write(fds[1], combined, request_len + 4) == (ssize_t)(request_len + 4));
    assert(sfu_ws_handshake(fds[0], &state) == 0);
    assert(write(fds[1], frame + 4, frame_len - 4) == (ssize_t)(frame_len - 4));

    char buf[64];
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == (ssize_t)strlen(payload));
    assert(strcmp(buf, payload) == 0);
    close(fds[0]);
    close(fds[1]);
  }

  /* Multiple frames coalesced with the upgrade remain ordered and pending
   * until each complete message is consumed. */
  {
    int fds[2];
    open_pair(fds);
    const char *request =
        "GET /chat HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    const uint8_t mask1[4] = {2, 4, 6, 8};
    const uint8_t mask2[4] = {8, 6, 4, 2};
    uint8_t frame1[32];
    uint8_t frame2[32];
    size_t frame1_len = build_masked_frame(frame1, 1, 0x1, "one", 3, mask1);
    size_t frame2_len = build_masked_frame(frame2, 1, 0x1, "two", 3, mask2);
    size_t request_len = strlen(request);
    uint8_t combined[512];
    memcpy(combined, request, request_len);
    memcpy(combined + request_len, frame1, frame1_len);
    memcpy(combined + request_len + frame1_len, frame2, frame2_len);
    size_t combined_len = request_len + frame1_len + frame2_len;
    assert(write(fds[1], combined, combined_len) == (ssize_t)combined_len);
    assert(sfu_ws_handshake(fds[0], &state) == 0);

    char buf[16];
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == 3);
    assert(strcmp(buf, "one") == 0);
    assert(sfu_ws_read_state_has_pending(&state));
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == 3);
    assert(strcmp(buf, "two") == 0);
    assert(!sfu_ws_read_state_has_pending(&state));
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
    ssize_t rn = sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf));
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
    ssize_t n = sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf));
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
    ssize_t n = sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf));
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
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == -1);
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
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == -1);
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
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == -1);
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
    assert(sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf)) == 0);
    close(fds[0]);
    close(fds[1]);
  }

  {
    int fds[2];
    open_pair(fds);
    set_nonblocking(fds[0]);
    sfu_ws_read_state_free(&state);
    memset(&state, 0, sizeof(state));

    const char *payload = "hello nonblocking streaming world!";
    size_t plen = strlen(payload);
    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t frame[128];
    size_t flen = build_masked_frame(frame, 1, 0x1, payload, plen, mask);

    char buf[128];
    for (size_t i = 0; i < flen; i++) {
      assert(write(fds[1], frame + i, 1) == 1);
      ssize_t n = sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf));
      if (i + 1 < flen) {
        assert(n == -1);
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
      } else {
        assert(n == (ssize_t)plen);
        assert(strcmp(buf, payload) == 0);
      }
    }

    close(fds[0]);
    close(fds[1]);
  }

  {
    int fds[2];
    open_pair(fds);
    set_nonblocking(fds[0]);
    sfu_ws_read_state_free(&state);
    memset(&state, 0, sizeof(state));

    char payload_16bit[300];
    for (size_t i = 0; i < sizeof(payload_16bit) - 1; i++) {
      payload_16bit[i] = (char)('a' + (i % 26));
    }
    payload_16bit[sizeof(payload_16bit) - 1] = '\0';
    size_t plen = strlen(payload_16bit);

    const uint8_t mask[4] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint8_t frame[512];
    size_t flen = build_masked_frame(frame, 1, 0x1, payload_16bit, plen, mask);

    char buf[512];
    for (size_t i = 0; i < flen; i++) {
      assert(write(fds[1], frame + i, 1) == 1);
      ssize_t n = sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf));
      if (i + 1 < flen) {
        assert(n == -1);
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
      } else {
        assert(n == (ssize_t)plen);
        assert(strcmp(buf, payload_16bit) == 0);
      }
    }

    close(fds[0]);
    close(fds[1]);
  }

  {
    int fds[2];
    open_pair(fds);
    set_nonblocking(fds[0]);
    sfu_ws_read_state_free(&state);
    memset(&state, 0, sizeof(state));

    const uint8_t mask1[4] = {1, 2, 3, 4};
    const uint8_t mask_ping[4] = {5, 6, 7, 8};
    const uint8_t mask2[4] = {9, 10, 11, 12};

    uint8_t f1[64];
    size_t f1_len = build_masked_frame(f1, 0, 0x1, "frag1_", 6, mask1);
    uint8_t f_ping[64];
    size_t f_ping_len = build_masked_frame(f_ping, 1, 0x9, "pingdata", 8, mask_ping);
    uint8_t f2[64];
    size_t f2_len = build_masked_frame(f2, 1, 0x0, "frag2", 5, mask2);

    uint8_t stream[256];
    size_t stream_len = 0;
    memcpy(stream + stream_len, f1, f1_len);
    stream_len += f1_len;
    memcpy(stream + stream_len, f_ping, f_ping_len);
    stream_len += f_ping_len;
    memcpy(stream + stream_len, f2, f2_len);
    stream_len += f2_len;

    char buf[64];
    for (size_t i = 0; i < stream_len; i++) {
      assert(write(fds[1], stream + i, 1) == 1);
      ssize_t res = sfu_ws_recv_text(fds[0], &state, buf, sizeof(buf));
      if (i + 1 < stream_len) {
        assert(res == -1);
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
      } else {
        assert(res == 11);
        assert(strcmp(buf, "frag1_frag2") == 0);
      }
    }

    uint8_t pong[32];
    ssize_t pn = read(fds[1], pong, sizeof(pong));
    assert(pn == 10);
    assert(pong[0] == 0x8A);
    assert(pong[1] == 8);
    assert(memcmp(pong + 2, "pingdata", 8) == 0);

    close(fds[0]);
    close(fds[1]);
  }

  {
    int fds[2];
    open_pair(fds);
    set_nonblocking(fds[0]);
    sfu_ws_read_state_free(&state);
    memset(&state, 0, sizeof(state));

    const uint8_t mask[4] = {0x55, 0x66, 0x77, 0x88};
    uint8_t frame[64];
    size_t frame_len = build_masked_frame(frame, 1, 0x1, "partial_msg", 11, mask);
    assert(write(fds[1], frame, 5) == 5);

    char buf1[64];
    assert(sfu_ws_recv_text(fds[0], &state, buf1, sizeof(buf1)) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    assert(write(fds[1], frame + 5, frame_len - 5) == (ssize_t)(frame_len - 5));
    char buf2[64];
    assert(sfu_ws_recv_text(fds[0], &state, buf2, sizeof(buf2)) == 11);
    assert(strcmp(buf2, "partial_msg") == 0);

    sfu_ws_read_state_free(&state);
    close(fds[0]);
    close(fds[1]);
  }

  {
    int fds[2];
    open_pair(fds);
    set_nonblocking(fds[0]);
    sfu_ws_read_state_free(&state);
    memset(&state, 0, sizeof(state));

    const uint8_t mask[4] = {0x55, 0x66, 0x77, 0x88};
    char payload[101];
    memset(payload, 'C', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    uint8_t frame[256];
    size_t frame_len = build_masked_frame(frame, 1, 0x1, payload, strlen(payload), mask);
    assert(write(fds[1], frame, 80) == 80);

    char large_buf[128];
    assert(sfu_ws_recv_text(fds[0], &state, large_buf, sizeof(large_buf)) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    assert(write(fds[1], frame + 80, frame_len - 80) == (ssize_t)(frame_len - 80));
    char small_buf[64];
    assert(sfu_ws_recv_text(fds[0], &state, small_buf, sizeof(small_buf)) == -1);
    assert(errno == EPROTO);

    sfu_ws_read_state_free(&state);
    close(fds[0]);
    close(fds[1]);
  }

  {
    int a[2];
    int b[2];
    open_pair(a);
    open_pair(b);
    set_nonblocking(a[0]);
    set_nonblocking(b[0]);
    sfu_ws_read_state_t state_a = {0};
    sfu_ws_read_state_t state_b = {0};

    const uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    const char *long_msg = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const char *short_msg = "BBBBBBBBBB";
    uint8_t frame_a[256];
    size_t len_a = build_masked_frame(frame_a, 1, 0x1, long_msg, strlen(long_msg), mask);
    uint8_t frame_b[256];
    size_t len_b = build_masked_frame(frame_b, 1, 0x1, short_msg, strlen(short_msg), mask);

    char shared[128];
    assert(write(a[1], frame_a, 20) == 20);
    assert(sfu_ws_recv_text(a[0], &state_a, shared, sizeof(shared)) == -1);
    assert(errno == EAGAIN || errno == EWOULDBLOCK);

    assert(write(b[1], frame_b, len_b) == (ssize_t)len_b);
    assert(sfu_ws_recv_text(b[0], &state_b, shared, sizeof(shared)) == (ssize_t)strlen(short_msg));
    assert(strcmp(shared, short_msg) == 0);

    assert(write(a[1], frame_a + 20, len_a - 20) == (ssize_t)(len_a - 20));
    assert(sfu_ws_recv_text(a[0], &state_a, shared, sizeof(shared)) == (ssize_t)strlen(long_msg));
    assert(strcmp(shared, long_msg) == 0);

    sfu_ws_read_state_free(&state_a);
    sfu_ws_read_state_free(&state_b);
    close(a[0]);
    close(a[1]);
    close(b[0]);
    close(b[1]);
  }

  sfu_ws_read_state_free(&state);
  printf("test_ws_handshake: OK\n");
  return 0;
}
