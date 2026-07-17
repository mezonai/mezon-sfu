#include "transport/srtp/srtp.h"

#include <assert.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

/* Builds a plausible-looking minimal RTP header + payload for testing. */
static int build_fake_rtp(uint8_t *buf, size_t cap, uint16_t seq, const char *payload) {
  size_t plen = strlen(payload);
  if (12 + plen > cap) {
    return -1;
  }
  buf[0] = 0x80; /* version 2, no padding/extension/csrc */
  buf[1] = 111;  /* payload type (arbitrary, Opus-ish) */
  buf[2] = (uint8_t)(seq >> 8);
  buf[3] = (uint8_t)(seq & 0xFF);
  memset(buf + 4, 0, 8); /* timestamp + SSRC, arbitrary for this test */
  memcpy(buf + 12, payload, plen);
  return (int)(12 + plen);
}

/* Directly builds a libsrtp session from raw key+salt dynamically based on the
 * profile. */
static int build_raw_session_dynamic(srtp_t *session, const uint8_t *key, size_t key_len, const uint8_t *salt, size_t salt_len, unsigned long profile_id,
                                     srtp_ssrc_type_t type) {
  uint8_t master[64];
  memcpy(master, key, key_len);
  memcpy(master + key_len, salt, salt_len);

  srtp_policy_t policy;
  memset(&policy, 0, sizeof(policy));

  if (profile_id == 0x0007) {  // SRTP_AEAD_AES_128_GCM
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
  } else if (profile_id == 0x0008) {  // SRTP_AEAD_AES_256_GCM
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtp);
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtcp);
  } else {  // Fallback/Default AES-CM-128
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
  }

  policy.ssrc.type = type;
  policy.key = master;
  policy.window_size = 128;

  return srtp_create(session, &policy) == srtp_err_status_ok ? 0 : -1;
}

static void run_srtp_test_for_profile(unsigned long profile_id, size_t key_len, size_t salt_len, bool is_server) {
  size_t total_material_len = (key_len * 2) + (salt_len * 2);
  uint8_t keying_material[88];  // Adequate buffer for any tested key lengths
  assert(total_material_len <= sizeof(keying_material));

  RAND_bytes(keying_material, total_material_len);

  const uint8_t *client_key = keying_material;
  const uint8_t *server_key = keying_material + key_len;
  const uint8_t *client_salt = keying_material + (key_len * 2);
  const uint8_t *server_salt = keying_material + (key_len * 2) + salt_len;

  /* Our SFU's session, using our dynamic init with role context */
  sfu_srtp_ctx_t sfu_ctx;
  assert(sfu_srtp_ctx_init_from_dtls(&sfu_ctx, keying_material, profile_id, is_server) == 0);

  /* Independent "client" sessions mirroring the destination.
   * If the SFU is the server, the remote client's outbound must use the client
   * keys. If the SFU is the client, the remote peer's outbound must use the
   * server keys. */
  srtp_t client_outbound, client_inbound;
  if (is_server) {
    assert(build_raw_session_dynamic(&client_outbound, client_key, key_len, client_salt, salt_len, profile_id, ssrc_any_outbound) == 0);
    assert(build_raw_session_dynamic(&client_inbound, server_key, key_len, server_salt, salt_len, profile_id, ssrc_any_inbound) == 0);
  } else {
    assert(build_raw_session_dynamic(&client_outbound, server_key, key_len, server_salt, salt_len, profile_id, ssrc_any_outbound) == 0);
    assert(build_raw_session_dynamic(&client_inbound, client_key, key_len, client_salt, salt_len, profile_id, ssrc_any_inbound) == 0);
  }

  /* Direction 1: client encrypts -> SFU decrypts */
  {
    uint8_t buf[200];
    int len = build_fake_rtp(buf, sizeof(buf), 1000, "hello-from-client");
    assert(len > 0);
    assert(srtp_protect(client_outbound, buf, &len) == srtp_err_status_ok);

    int plain_len = len;
    assert(sfu_srtp_unprotect_rtp(&sfu_ctx, buf, &plain_len));
    assert(plain_len == 12 + (int)strlen("hello-from-client"));
    assert(memcmp(buf + 12, "hello-from-client", strlen("hello-from-client")) == 0);
  }

  /* Direction 2: SFU encrypts -> client decrypts */
  {
    uint8_t buf[200];
    int len = build_fake_rtp(buf, sizeof(buf), 2000, "hello-from-sfu");
    assert(len > 0);
    int cap = (int)sizeof(buf);
    assert(sfu_srtp_protect_rtp(&sfu_ctx, buf, &len, (size_t)cap));

    int plain_len = len;
    assert(srtp_unprotect(client_inbound, buf, &plain_len) == srtp_err_status_ok);
    assert(plain_len == 12 + (int)strlen("hello-from-sfu"));
    assert(memcmp(buf + 12, "hello-from-sfu", strlen("hello-from-sfu")) == 0);
  }

  /* Tampered ciphertext test */
  {
    uint8_t buf[200];
    int len = build_fake_rtp(buf, sizeof(buf), 3000, "tamper-me");
    assert(srtp_protect(client_outbound, buf, &len) == srtp_err_status_ok);
    buf[15] ^= 0xFF; /* flip a payload byte */

    int plain_len = len;
    assert(sfu_srtp_unprotect_rtp(&sfu_ctx, buf, &plain_len) == false);
  }

  srtp_dealloc(client_outbound);
  srtp_dealloc(client_inbound);
  sfu_srtp_ctx_destroy(&sfu_ctx);
}

int main(void) {
  assert(sfu_srtp_global_init() == 0);

  /* Run tests simulating SFU as BOTH DTLS Server and DTLS Client */
  for (int role_server = 0; role_server <= 1; role_server++) {
    bool is_server = (role_server == 1);

    /* Test 1: Fallback AES-CM-128 profile (Profile ID: 0x0001, 16-byte key,
     * 14-byte salt) */
    run_srtp_test_for_profile(0x0001, 16, 14, is_server);

    /* Test 2: GCM-128 profile (Profile ID: 0x0007, 16-byte key, 12-byte salt)
     */
    run_srtp_test_for_profile(0x0007, 16, 12, is_server);

    /* Test 3: GCM-256 profile (Profile ID: 0x0008, 32-byte key, 12-byte salt)
     */
    run_srtp_test_for_profile(0x0008, 32, 12, is_server);
  }

  /* Test 4: RTP/RTCP classification */
  {
    uint8_t rtcp_pkt[8] = {0x80, 200, 0, 0, 0, 0, 0, 0};
    uint8_t rtp_pkt[8] = {0x80, 111, 0, 0, 0, 0, 0, 0};
    assert(sfu_rtp_is_rtcp(rtcp_pkt, sizeof(rtcp_pkt)) == true);
    assert(sfu_rtp_is_rtcp(rtp_pkt, sizeof(rtp_pkt)) == false);
  }

  sfu_srtp_global_deinit();

  printf("test_srtp: OK\n");
  return 0;
}
