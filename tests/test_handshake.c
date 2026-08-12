#include <assert.h>
#include <inttypes.h>
#include <openssl/digest.h>
#include <openssl/hmac.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "protocol/signaling/handshake.h"

static void b64url_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < in_len; i += 3) {
    uint32_t n = ((uint32_t)in[i]) << 16;
    if (i + 1 < in_len) {
      n |= ((uint32_t)in[i + 1]) << 8;
    }
    if (i + 2 < in_len) {
      n |= (uint32_t)in[i + 2];
    }
    char b0 = tbl[(n >> 18) & 63];
    char b1 = tbl[(n >> 12) & 63];
    char b2 = (i + 1 < in_len) ? tbl[(n >> 6) & 63] : '=';
    char b3 = (i + 2 < in_len) ? tbl[n & 63] : '=';
    if (o + 4 >= out_cap) {
      out[0] = '\0';
      return;
    }
    out[o++] = b0;
    out[o++] = b1;
    out[o++] = b2;
    out[o++] = b3;
  }
  /* Strip padding and convert to base64url. */
  while (o > 0 && out[o - 1] == '=') {
    o--;
  }
  for (size_t i = 0; i < o; i++) {
    if (out[i] == '+') {
      out[i] = '-';
    } else if (out[i] == '/') {
      out[i] = '_';
    }
  }
  out[o] = '\0';
}

static int mint_hs256(const char *header_json, const char *payload_json, const char *secret, char *out, size_t out_cap) {
  char h_b64[256];
  char p_b64[512];
  b64url_encode((const uint8_t *)header_json, strlen(header_json), h_b64, sizeof(h_b64));
  b64url_encode((const uint8_t *)payload_json, strlen(payload_json), p_b64, sizeof(p_b64));

  char signing[768];
  int si = snprintf(signing, sizeof(signing), "%s.%s", h_b64, p_b64);
  if (si < 0 || (size_t)si >= sizeof(signing)) {
    return -1;
  }

  uint8_t mac[EVP_MAX_MD_SIZE];
  unsigned int mac_len = 0;
  if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), (const uint8_t *)signing, (size_t)si, mac, &mac_len)) {
    return -1;
  }

  char s_b64[128];
  b64url_encode(mac, mac_len, s_b64, sizeof(s_b64));

  int n = snprintf(out, out_cap, "%s.%s", signing, s_b64);
  return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

int main(void) {
  const char *secret = "test-jwt-secret";
  char token[1024];

  /* Valid token with uid + exp. */
  {
    char payload[128];
    int64_t exp = (int64_t)time(NULL) + 3600;
    snprintf(payload, sizeof(payload), "{\"tid\":\"t1\",\"uid\":42,\"usn\":\"alice\",\"exp\":%" PRId64 "}", exp);
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);

    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, &user_id) == 0);
    assert(user_id == 42);

    sfu_jwt_claims_t claims;
    assert(sfu_jwt_parse_hs256(token, strlen(token), secret, strlen(secret), &claims) == 0);
    assert(claims.has_user_id && claims.user_id == 42);
    assert(claims.has_exp && claims.exp == exp);
  }

  /* user_id claim fallback (string). */
  {
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", "{\"user_id\":\"99\"}", secret, token, sizeof(token)) == 0);
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, &user_id) == 0);
    assert(user_id == 99);
  }

  /* Bad signature. */
  {
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", "{\"uid\":1}", secret, token, sizeof(token)) == 0);
    size_t len = strlen(token);
    token[len - 1] = (token[len - 1] == 'A') ? 'B' : 'A';
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, &user_id) != 0);
  }

  /* Expired. */
  {
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"uid\":7,\"exp\":1}");
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, &user_id) != 0);
  }

  /* Wrong alg. */
  {
    assert(mint_hs256("{\"alg\":\"none\",\"typ\":\"JWT\"}", "{\"uid\":1}", secret, token, sizeof(token)) == 0);
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, &user_id) != 0);
  }

  printf("test_handshake: all passed\n");
  return 0;
}
