#ifndef SFU_PROTOCOL_HANDSHAKE_H
#define SFU_PROTOCOL_HANDSHAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sfu_jwt_claims {
  int64_t user_id;
  int64_t exp;
  int64_t nbf;
  uint64_t room_id;
  char iss[64];
  char metadata[64];
  bool room_join;
  bool has_user_id;
  bool has_exp;
  bool has_nbf;
  bool has_room;
} sfu_jwt_claims_t;

int sfu_jwt_parse_hs256(const char *token, size_t token_len, const char *secret, size_t secret_len, sfu_jwt_claims_t *out);
int sfu_handshake_verify_token_claims(const char *token, size_t token_len, const char *secret, sfu_jwt_claims_t *out);
int sfu_handshake_verify_join_token(const char *token, size_t token_len, const char *secret, int64_t *out_user_id, uint64_t *out_room_id);

#endif /* SFU_PROTOCOL_HANDSHAKE_H */
