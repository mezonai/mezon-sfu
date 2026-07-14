#include "transport/srtp/srtp.h"

#include <assert.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

/* Builds a plausible-looking minimal RTP header + payload for testing.
 * Real validity (padding/extension bits etc.) doesn't matter to SRTP;
 * it treats the header as opaque auth-covered bytes past the fixed
 * 12-byte prefix. */
static int build_fake_rtp(uint8_t *buf, size_t cap, uint16_t seq,
                          const char *payload) {
  size_t plen = strlen(payload);
  if (12 + plen > cap)
    return -1;
  buf[0] = 0x80; /* version 2, no padding/extension/csrc */
  buf[1] = 111;  /* payload type (arbitrary, Opus-ish) */
  buf[2] = (uint8_t)(seq >> 8);
  buf[3] = (uint8_t)(seq & 0xFF);
  memset(buf + 4, 0, 8); /* timestamp + SSRC, arbitrary for this test */
  memcpy(buf + 12, payload, plen);
  return (int)(12 + plen);
}

/* Directly builds a libsrtp session from raw key+salt, mirroring what a
 * "client" endpoint would do with its own half of the DTLS-exported
 * material -- used to independently verify our key-splitting logic
 * rather than just testing our wrapper against itself. */
static int build_raw_session(srtp_t *session, const uint8_t *key16,
                             const uint8_t *salt14, srtp_ssrc_type_t type) {
  uint8_t master[30];
  memcpy(master, key16, 16);
  memcpy(master + 16, salt14, 14);

  srtp_policy_t policy;
  memset(&policy, 0, sizeof(policy));
  srtp_crypto_policy_set_rtp_default(&policy.rtp);
  srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
  policy.ssrc.type = type;
  policy.key = master;
  policy.window_size = 128;

  return srtp_create(session, &policy) == srtp_err_status_ok ? 0 : -1;
}

int main(void) {
  assert(sfu_srtp_global_init() == 0);

  uint8_t keying_material[60];
  RAND_bytes(keying_material, sizeof(keying_material));

  const uint8_t *client_key = keying_material;
  const uint8_t *server_key = keying_material + 16;
  const uint8_t *client_salt = keying_material + 32;
  const uint8_t *server_salt = keying_material + 46;

  /* Our SFU's session, as if this were the result of a completed
   * DTLS handshake where we were the server. */
  sfu_srtp_ctx_t sfu_ctx;
  assert(sfu_srtp_ctx_init_from_dtls(&sfu_ctx, keying_material) == 0);

  /* An independent "client" pair of sessions built directly from the
   * same material's client/server halves, exactly as a real WebRTC
   * client's DTLS stack would derive them. */
  srtp_t client_outbound, client_inbound;
  assert(build_raw_session(&client_outbound, client_key, client_salt,
                           ssrc_any_outbound) == 0);
  assert(build_raw_session(&client_inbound, server_key, server_salt,
                           ssrc_any_inbound) == 0);

  /* Direction 1: client encrypts (its outbound = client_write key),
   * our SFU decrypts (its inbound = client_write key). Must round-trip. */
  {
    uint8_t buf[200];
    int len = build_fake_rtp(buf, sizeof(buf), 1000, "hello-from-client");
    assert(len > 0);
    assert(srtp_protect(client_outbound, buf, &len) == srtp_err_status_ok);

    int plain_len = len;
    assert(sfu_srtp_unprotect_rtp(&sfu_ctx, buf, &plain_len));
    assert(plain_len == 12 + (int)strlen("hello-from-client"));
    assert(memcmp(buf + 12, "hello-from-client", strlen("hello-from-client")) ==
           0);
  }

  /* Direction 2: our SFU encrypts (its outbound = server_write key),
   * client decrypts (its inbound = server_write key). Must round-trip. */
  {
    uint8_t buf[200];
    int len = build_fake_rtp(buf, sizeof(buf), 2000, "hello-from-sfu");
    assert(len > 0);
    int cap = (int)sizeof(buf);
    assert(sfu_srtp_protect_rtp(&sfu_ctx, buf, &len, (size_t)cap));

    int plain_len = len;
    assert(srtp_unprotect(client_inbound, buf, &plain_len) ==
           srtp_err_status_ok);
    assert(plain_len == 12 + (int)strlen("hello-from-sfu"));
    assert(memcmp(buf + 12, "hello-from-sfu", strlen("hello-from-sfu")) == 0);
  }

  /* Tampered ciphertext must fail authentication, not silently decrypt
   * to garbage -- this is the entire point of the auth tag. */
  {
    uint8_t buf[200];
    int len = build_fake_rtp(buf, sizeof(buf), 3000, "tamper-me");
    assert(srtp_protect(client_outbound, buf, &len) == srtp_err_status_ok);
    buf[15] ^= 0xFF; /* flip a payload byte */

    int plain_len = len;
    assert(sfu_srtp_unprotect_rtp(&sfu_ctx, buf, &plain_len) == false);
  }

  /* RTP/RTCP classification: SR (200) and RR (201) are RTCP; a
   * regular RTP payload type is not. */
  {
    uint8_t rtcp_pkt[8] = {0x80, 200, 0, 0, 0, 0, 0, 0};
    uint8_t rtp_pkt[8] = {0x80, 111, 0, 0, 0, 0, 0, 0};
    assert(sfu_rtp_is_rtcp(rtcp_pkt, sizeof(rtcp_pkt)) == true);
    assert(sfu_rtp_is_rtcp(rtp_pkt, sizeof(rtp_pkt)) == false);
  }

  srtp_dealloc(client_outbound);
  srtp_dealloc(client_inbound);
  sfu_srtp_ctx_destroy(&sfu_ctx);
  sfu_srtp_global_deinit();

  printf("test_srtp: OK\n");
  return 0;
}
