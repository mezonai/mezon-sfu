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
  char p_b64[768];
  b64url_encode((const uint8_t *)header_json, strlen(header_json), h_b64, sizeof(h_b64));
  b64url_encode((const uint8_t *)payload_json, strlen(payload_json), p_b64, sizeof(p_b64));

  char signing[1024];
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

  /* Valid token with identity + video.room + roomJoin. */
  {
    char payload[384];
    int64_t exp = (int64_t)time(NULL) + 3600;
    int64_t nbf = (int64_t)time(NULL) - 60;
    snprintf(payload, sizeof(payload),
             "{\"exp\":%" PRId64 ",\"identity\":\"1843252237590073344\",\"iss\":\"APIcdd28PzB2amp\",\"metadata\":\"longma350\","
             "\"nbf\":%" PRId64 ",\"sub\":\"1843252237590073344\",\"video\":{\"room\":\"2087757797482565632\",\"roomJoin\":true}}",
             exp, nbf);
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);

    int64_t user_id = 0;
    uint64_t room_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, 2087757797482565632ULL, &user_id, &room_id) == 0);
    assert(user_id == 1843252237590073344LL);
    assert(room_id == 2087757797482565632ULL);

    sfu_jwt_claims_t claims;
    assert(sfu_jwt_parse_hs256(token, strlen(token), secret, strlen(secret), &claims) == 0);
    assert(claims.has_user_id && claims.user_id == 1843252237590073344LL);
    assert(claims.has_exp && claims.exp == exp);
    assert(claims.has_nbf && claims.nbf == nbf);
    assert(claims.has_room && claims.room_id == 2087757797482565632ULL);
    assert(claims.room_join);
    assert(strcmp(claims.iss, "APIcdd28PzB2amp") == 0);
    assert(strcmp(claims.metadata, "longma350") == 0);
  }

  /* sub fallback when identity is absent. */
  {
    char payload[256];
    int64_t exp = (int64_t)time(NULL) + 3600;
    snprintf(payload, sizeof(payload), "{\"sub\":\"99\",\"exp\":%" PRId64 ",\"video\":{\"room\":\"101\",\"roomJoin\":true}}", exp);
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);
    int64_t user_id = 0;
    uint64_t room_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, 0, &user_id, &room_id) == 0);
    assert(user_id == 99);
    assert(room_id == 101);
  }

  /* Room mismatch. */
  {
    char payload[256];
    int64_t exp = (int64_t)time(NULL) + 3600;
    snprintf(payload, sizeof(payload), "{\"identity\":\"1\",\"exp\":%" PRId64 ",\"video\":{\"room\":\"101\",\"roomJoin\":true}}", exp);
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, 202ULL, &user_id, NULL) != 0);
  }

  /* roomJoin false. */
  {
    char payload[256];
    int64_t exp = (int64_t)time(NULL) + 3600;
    snprintf(payload, sizeof(payload), "{\"identity\":\"1\",\"exp\":%" PRId64 ",\"video\":{\"room\":\"101\",\"roomJoin\":false}}", exp);
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, 101ULL, &user_id, NULL) != 0);
  }

  /* Bad signature. */
  {
    char payload[256];
    int64_t exp = (int64_t)time(NULL) + 3600;
    snprintf(payload, sizeof(payload), "{\"identity\":\"1\",\"exp\":%" PRId64 ",\"video\":{\"room\":\"101\",\"roomJoin\":true}}", exp);
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);
    size_t len = strlen(token);
    token[len - 2] = (token[len - 2] == 'A') ? 'B' : 'A';
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, 101ULL, &user_id, NULL) != 0);
  }

  /* Expired. */
  {
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"identity\":\"7\",\"exp\":1,\"video\":{\"room\":\"101\",\"roomJoin\":true}}");
    assert(mint_hs256("{\"alg\":\"HS256\",\"typ\":\"JWT\"}", payload, secret, token, sizeof(token)) == 0);
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, 101ULL, &user_id, NULL) != 0);
  }

  /* Wrong alg. */
  {
    assert(mint_hs256("{\"alg\":\"none\",\"typ\":\"JWT\"}", "{\"identity\":\"1\",\"video\":{\"room\":\"101\",\"roomJoin\":true}}", secret, token,
                      sizeof(token)) == 0);
    int64_t user_id = 0;
    assert(sfu_handshake_verify_join_token(token, strlen(token), secret, 101ULL, &user_id, NULL) != 0);
  }

  printf("test_handshake: all passed\n");
  return 0;
}
