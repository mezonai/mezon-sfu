#include "transport/stun/stun.h"

#include <arpa/inet.h>
#include <assert.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <string.h>

static void write_be16(uint8_t *p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v & 0xFF;
}
static void write_be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

/* Builds a minimal, correctly-authenticated Binding Request as a real
 * ICE client would: USERNAME = "{server-ufrag}:{client-ufrag}",
 * MESSAGE-INTEGRITY keyed with the server's password (the short-term
 * credential rule: authenticate with the *recipient's* password). */
static size_t build_binding_request(uint8_t *buf,
                                    const sfu_ice_credentials_t *server_creds,
                                    const char *client_ufrag) {
  write_be16(buf, 0x0001); /* Binding Request */
  write_be16(buf + 2, 0);  /* length, patched below */
  write_be32(buf + 4, 0x2112A442u);
  for (int i = 0; i < 12; i++)
    buf[8 + i] = (uint8_t)(0xA0 + i); /* arbitrary transaction id */

  size_t off = 20;
  char username[128];
  int ulen = snprintf(username, sizeof(username), "%s:%s", server_creds->ufrag,
                      client_ufrag);

  write_be16(buf + off, 0x0006); /* USERNAME */
  write_be16(buf + off + 2, (uint16_t)ulen);
  memcpy(buf + off + 4, username, (size_t)ulen);
  size_t padded = (size_t)((ulen + 3) & ~3);
  off += 4 + padded;

  write_be16(buf + 2, (uint16_t)((off - 20) + 24)); /* length up through M-I */
  uint8_t hmac[20];
  unsigned int hmac_len = 0;
  HMAC(EVP_sha1(), server_creds->pwd, (int)strlen(server_creds->pwd), buf, off,
       hmac, &hmac_len);
  write_be16(buf + off, 0x0008);
  write_be16(buf + off + 2, 20);
  memcpy(buf + off + 4, hmac, 20);
  off += 4 + 20;

  return off;
}

int main(void) {
  sfu_ice_credentials_t server_creds;
  sfu_ice_credentials_generate(&server_creds);
  assert(strlen(server_creds.ufrag) >= 4);
  assert(strlen(server_creds.pwd) >= 22);

  uint8_t request[512];
  size_t request_len =
      build_binding_request(request, &server_creds, "peerufrag");

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = inet_addr("203.0.113.7");
  sin.sin_port = htons(54321);
  struct sockaddr_storage src;
  memset(&src, 0, sizeof(src));
  memcpy(&src, &sin, sizeof(sin));

  uint8_t response[512];
  size_t response_len =
      sfu_stun_handle_binding_request(request, request_len, &server_creds, &src,
                                      sizeof(sin), response, sizeof(response));

  assert(response_len > 20);
  assert(sfu_stun_is_stun_packet(response, response_len));

  /* Response type must be Binding Success (0x0101). */
  uint16_t resp_type = ((uint16_t)response[0] << 8) | response[1];
  assert(resp_type == 0x0101);

  /* Transaction ID must be echoed exactly. */
  assert(memcmp(response + 8, request + 8, 12) == 0);

  /* Find MESSAGE-INTEGRITY in the response and independently verify
   * it with the same key a real client would use -- proving the
   * response is genuinely authenticated, not just non-empty. */
  size_t off = 20, mi_off = 0;
  while (off + 4 <= response_len) {
    uint16_t t = ((uint16_t)response[off] << 8) | response[off + 1];
    uint16_t l = ((uint16_t)response[off + 2] << 8) | response[off + 3];
    if (t == 0x0008) {
      mi_off = off;
      break;
    }
    off += 4 + ((l + 3) & ~3);
  }
  assert(mi_off > 0);

  uint8_t scratch[512];
  memcpy(scratch, response, mi_off);
  write_be16(scratch + 2, (uint16_t)((mi_off - 20) + 24));
  uint8_t expected_hmac[20];
  unsigned int expected_len = 0;
  HMAC(EVP_sha1(), server_creds.pwd, (int)strlen(server_creds.pwd), scratch,
       mi_off, expected_hmac, &expected_len);
  assert(memcmp(expected_hmac, response + mi_off + 4, 20) == 0);

  /* A request with a mismatched password must be rejected (no
   * response), proving the integrity check actually gates. */
  sfu_ice_credentials_t wrong_creds = server_creds;
  strcpy(wrong_creds.pwd, "totallyWrongPasswordValueXXXXX");
  uint8_t bad_request[512];
  size_t bad_len =
      build_binding_request(bad_request, &wrong_creds, "peerufrag");
  size_t bad_response_len =
      sfu_stun_handle_binding_request(bad_request, bad_len, &server_creds, &src,
                                      sizeof(sin), response, sizeof(response));
  assert(bad_response_len == 0);

  printf("test_stun: OK\n");
  return 0;
}
