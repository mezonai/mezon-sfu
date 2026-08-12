#ifndef SFU_PROTOCOL_HANDSHAKE_H
#define SFU_PROTOCOL_HANDSHAKE_H

#include <stddef.h>
#include <stdint.h>

typedef struct sfu_jwt_claims {
  int64_t user_id;
  int64_t exp;
  int has_user_id;
  int has_exp;
} sfu_jwt_claims_t;

int sfu_jwt_parse_hs256(const char *token, size_t token_len, const char *secret, size_t secret_len, sfu_jwt_claims_t *out);

int sfu_handshake_verify_join_token(const char *token, size_t token_len, const char *secret, int64_t *out_user_id);

#endif /* SFU_PROTOCOL_HANDSHAKE_H */
