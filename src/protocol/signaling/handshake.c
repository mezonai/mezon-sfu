#include "protocol/signaling/handshake.h"

#include <ctype.h>
#include <inttypes.h>
#include <openssl/base64.h>
#include <openssl/digest.h>
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "protocol/signaling/json_lite.h"
#include "util/log.h"

#define SFU_JWT_MAX_TOKEN 4096
#define SFU_JWT_MAX_PART 3072

static int b64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
  if (!in || !out || !out_len || out_cap == 0) {
    return -1;
  }

  char std[SFU_JWT_MAX_PART + 4];
  if (in_len > SFU_JWT_MAX_PART) {
    return -1;
  }

  size_t n = 0;
  for (size_t i = 0; i < in_len; i++) {
    char c = in[i];
    if (c == '-') {
      c = '+';
    } else if (c == '_') {
      c = '/';
    } else if (!(isalnum((unsigned char)c) || c == '+' || c == '/')) {
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        continue;
      }
      return -1;
    }
    std[n++] = c;
  }

  while (n % 4 != 0) {
    if (n + 1 >= sizeof(std)) {
      return -1;
    }
    std[n++] = '=';
  }
  std[n] = '\0';

  size_t max_decoded = (n / 4) * 3;
  if (max_decoded > out_cap) {
    return -1;
  }

  int decoded = EVP_DecodeBlock(out, (const uint8_t *)std, (int)n);
  if (decoded < 0) {
    return -1;
  }

  size_t pad = 0;
  if (n >= 1 && std[n - 1] == '=') {
    pad++;
  }
  if (n >= 2 && std[n - 2] == '=') {
    pad++;
  }
  if ((size_t)decoded < pad) {
    return -1;
  }
  *out_len = (size_t)decoded - pad;
  return 0;
}

static int extract_json_int64(const char *json, size_t json_len, const char *field, int64_t *out) {
  if (!json || !field || !out) {
    return -1;
  }

  char needle[96];
  int needle_len = snprintf(needle, sizeof(needle), "\"%s\"", field);
  if (needle_len < 0 || (size_t)needle_len >= sizeof(needle)) {
    return -1;
  }

  const char *end = json + json_len;
  for (const char *p = json; p + needle_len <= end; p++) {
    if (memcmp(p, needle, (size_t)needle_len) != 0) {
      continue;
    }
    p += needle_len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == ':')) {
      p++;
    }
    if (p >= end) {
      return -1;
    }

    if (*p == '"') {
      char buf[32];
      if (sfu_json_extract_string(json, json_len, field, buf, sizeof(buf)) < 0) {
        return -1;
      }
      char *endp = NULL;
      long long v = strtoll(buf, &endp, 10);
      if (!endp || *endp != '\0') {
        return -1;
      }
      *out = (int64_t)v;
      return 0;
    }

    if (*p != '-' && !isdigit((unsigned char)*p)) {
      return -1;
    }
    char *endp = NULL;
    long long v = strtoll(p, &endp, 10);
    if (!endp || endp == p) {
      return -1;
    }
    *out = (int64_t)v;
    return 0;
  }
  return -1;
}

int sfu_jwt_parse_hs256(const char *token, size_t token_len, const char *secret, size_t secret_len, sfu_jwt_claims_t *out) {
  if (!token || token_len == 0 || !secret || secret_len == 0 || !out) {
    return -1;
  }
  if (token_len > SFU_JWT_MAX_TOKEN) {
    return -1;
  }

  memset(out, 0, sizeof(*out));

  const char *dot1 = memchr(token, '.', token_len);
  if (!dot1) {
    return -1;
  }
  const char *dot2 = memchr(dot1 + 1, '.', (size_t)(token + token_len - (dot1 + 1)));
  if (!dot2) {
    return -1;
  }

  if (memchr(dot2 + 1, '.', (size_t)(token + token_len - (dot2 + 1)))) {
    return -1;
  }

  size_t header_len = (size_t)(dot1 - token);
  size_t payload_len = (size_t)(dot2 - (dot1 + 1));
  size_t sig_len = (size_t)(token + token_len - (dot2 + 1));
  if (header_len == 0 || payload_len == 0 || sig_len == 0) {
    return -1;
  }

  size_t signing_input_len = header_len + 1 + payload_len;

  uint8_t expected[EVP_MAX_MD_SIZE];
  unsigned int expected_len = 0;
  if (!HMAC(EVP_sha256(), secret, (int)secret_len, (const uint8_t *)token, signing_input_len, expected, &expected_len) || expected_len == 0) {
    return -1;
  }

  uint8_t actual[EVP_MAX_MD_SIZE];
  size_t actual_len = 0;
  if (b64url_decode(dot2 + 1, sig_len, actual, sizeof(actual), &actual_len) != 0) {
    return -1;
  }
  if (actual_len != expected_len || CRYPTO_memcmp(actual, expected, expected_len) != 0) {
    return -1;
  }

  uint8_t header_json[SFU_JWT_MAX_PART];
  size_t header_json_len = 0;
  if (b64url_decode(token, header_len, header_json, sizeof(header_json) - 1, &header_json_len) != 0) {
    return -1;
  }
  header_json[header_json_len] = '\0';

  char alg[16] = {0};
  if (sfu_json_extract_string((const char *)header_json, header_json_len, "alg", alg, sizeof(alg)) < 0) {
    return -1;
  }
  if (strcmp(alg, "HS256") != 0) {
    return -1;
  }

  uint8_t payload_json[SFU_JWT_MAX_PART];
  size_t payload_json_len = 0;
  if (b64url_decode(dot1 + 1, payload_len, payload_json, sizeof(payload_json) - 1, &payload_json_len) != 0) {
    return -1;
  }
  payload_json[payload_json_len] = '\0';

  int64_t user_id = 0;
  if (extract_json_int64((const char *)payload_json, payload_json_len, "identity", &user_id) == 0 ||
      extract_json_int64((const char *)payload_json, payload_json_len, "sub", &user_id) == 0) {
    out->user_id = user_id;
  }

  int64_t exp = 0;
  if (extract_json_int64((const char *)payload_json, payload_json_len, "exp", &exp) == 0) {
    out->exp = exp;
  }

  int64_t nbf = 0;
  if (extract_json_int64((const char *)payload_json, payload_json_len, "nbf", &nbf) == 0) {
    out->nbf = nbf;
  }

  (void)sfu_json_extract_string((const char *)payload_json, payload_json_len, "iss", out->iss, sizeof(out->iss));
  (void)sfu_json_extract_string((const char *)payload_json, payload_json_len, "metadata", out->metadata, sizeof(out->metadata));

  int64_t room_id = 0;
  if (extract_json_int64((const char *)payload_json, payload_json_len, "room", &room_id) == 0 && room_id > 0) {
    out->room_id = (uint64_t)room_id;
  }

  return 0;
}

int sfu_handshake_verify_token_claims(const char *token, size_t token_len, const char *secret, sfu_jwt_claims_t *out) {
  if (!token || token_len == 0 || !secret || secret[0] == '\0' || !out) {
    return -1;
  }

  sfu_jwt_claims_t claims;
  if (sfu_jwt_parse_hs256(token, token_len, secret, strlen(secret), &claims) != 0) {
    SFU_LOG_WARN("handshake: JWT parse/verify failed");
    return -1;
  }
  if (claims.user_id == 0) {
    SFU_LOG_WARN("handshake: JWT missing identity/sub claim");
    return -1;
  }
  if (claims.room_id == 0) {
    SFU_LOG_WARN("handshake: JWT missing video.room claim");
    return -1;
  }

  int64_t now = (int64_t)time(NULL);
  if (now < claims.nbf) {
    SFU_LOG_WARN("handshake: JWT not yet valid (nbf=%" PRId64 " now=%" PRId64 ")", claims.nbf, now);
    return -1;
  }
  if (claims.exp < now) {
    SFU_LOG_WARN("handshake: JWT expired (exp=%" PRId64 " now=%" PRId64 ")", claims.exp, now);
    return -1;
  }

  *out = claims;
  return 0;
}

int sfu_handshake_verify_join_token(const char *token, size_t token_len, const char *secret, int64_t *out_user_id, uint64_t *out_room_id) {
  if (!out_user_id || !out_room_id) {
    return -1;
  }

  sfu_jwt_claims_t claims;
  if (sfu_handshake_verify_token_claims(token, token_len, secret, &claims) != 0) {
    return -1;
  }

  *out_user_id = claims.user_id;
  *out_room_id = claims.room_id;
  return 0;
}
