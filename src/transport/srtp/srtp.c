#include "transport/srtp/srtp.h"
#include "util/log.h"

#include <string.h>

#define SFU_SRTP_MASTER_KEY_LEN 16
#define SFU_SRTP_MASTER_SALT_LEN 14

int sfu_srtp_global_init(void) {
  srtp_err_status_t rc = srtp_init();
  if (rc != srtp_err_status_ok) {
    SFU_LOG_ERROR("srtp_init failed: %d", (int)rc);
    return -1;
  }
  return 0;
}

void sfu_srtp_global_deinit(void) { srtp_shutdown(); }

static int create_session_dynamic(srtp_t *session, const uint8_t *master_key, unsigned long profile_id, srtp_ssrc_type_t ssrc_type) {
  srtp_policy_t policy;
  memset(&policy, 0, sizeof(policy));

  if (profile_id == 0x0007) {  // SRTP_AEAD_AES_128_GCM
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
  } else if (profile_id == 0x0008) {  // SRTP_AEAD_AES_256_GCM
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtp);
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy.rtcp);
  } else {  // Fallback to SRTP_AES128_CM_SHA1_80 (0x0001)
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
  }

  policy.ssrc.type = ssrc_type;
  policy.ssrc.value = 0;
  policy.key = (uint8_t *)master_key;
  policy.next = NULL;
  policy.window_size = 1024;
  // Allow duplicate/reordered packet processing without throwing fatal crypto errors
  policy.allow_repeat_tx = 1;

  srtp_err_status_t rc = srtp_create(session, &policy);
  if (rc != srtp_err_status_ok) {
    SFU_LOG_ERROR("srtp_create failed: %d", (int)rc);
    return -1;
  }
  return 0;
}

int sfu_srtp_ctx_init_from_dtls(sfu_srtp_ctx_t *ctx, const uint8_t *keying_material, unsigned long profile_id, bool is_server) {
  memset(ctx, 0, sizeof(*ctx));

  int key_len = 16;
  int salt_len = 14;

  if (profile_id == 0x0007) {  // GCM-128
    key_len = 16;
    salt_len = 12;
  } else if (profile_id == 0x0008) {  // GCM-256
    key_len = 32;
    salt_len = 12;
  }

  // Layout: [client_key][server_key][client_salt][server_salt]
  const uint8_t *client_key = keying_material;
  const uint8_t *server_key = keying_material + key_len;
  const uint8_t *client_salt = keying_material + (key_len * 2);
  const uint8_t *server_salt = keying_material + (key_len * 2) + salt_len;

  uint8_t client_master[64], server_master[64];

  // Concatenate keys and salts
  memcpy(client_master, client_key, key_len);
  memcpy(client_master + key_len, client_salt, salt_len);

  memcpy(server_master, server_key, key_len);
  memcpy(server_master + key_len, server_salt, salt_len);

  int rc1, rc2;
  if (is_server) {
    // If we are Server: Inbound = Client, Outbound = Server
    rc1 = create_session_dynamic(&ctx->inbound, client_master, profile_id, ssrc_any_inbound);
    rc2 = create_session_dynamic(&ctx->outbound, server_master, profile_id, ssrc_any_outbound);
  } else {
    // If we are Client: Inbound = Server, Outbound = Client
    rc1 = create_session_dynamic(&ctx->inbound, server_master, profile_id, ssrc_any_inbound);
    rc2 = create_session_dynamic(&ctx->outbound, client_master, profile_id, ssrc_any_outbound);
  }

  if (rc1 != 0 || rc2 != 0) {
    sfu_srtp_ctx_destroy(ctx);
    return -1;
  }

  return 0;
}

void sfu_srtp_ctx_destroy(sfu_srtp_ctx_t *ctx) {
  if (ctx->inbound) {
    srtp_dealloc(ctx->inbound);
    ctx->inbound = NULL;
  }
  if (ctx->outbound) {
    srtp_dealloc(ctx->outbound);
    ctx->outbound = NULL;
  }
}

bool sfu_srtp_unprotect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len) {
  srtp_err_status_t rc = srtp_unprotect(ctx->inbound, buf, len);
  if (rc != srtp_err_status_ok) {
    SFU_LOG_WARN("SRTP unprotect (RTP) failed: %d", (int)rc);
    return false;
  }
  return true;
}

const char *sfu_srtp_status_name(srtp_err_status_t status) {
  switch (status) {
    case srtp_err_status_ok:
      return "ok";
    case srtp_err_status_fail:
      return "fail";
    case srtp_err_status_bad_param:
      return "bad_param";
    case srtp_err_status_alloc_fail:
      return "alloc_fail";
    case srtp_err_status_dealloc_fail:
      return "dealloc_fail";
    case srtp_err_status_init_fail:
      return "init_fail";
    case srtp_err_status_terminus:
      return "terminus";
    case srtp_err_status_auth_fail:
      return "auth_fail";
    case srtp_err_status_cipher_fail:
      return "cipher_fail";
    case srtp_err_status_replay_fail:
      return "replay_fail";
    case srtp_err_status_replay_old:
      return "replay_old";
    case srtp_err_status_algo_fail:
      return "algo_fail";
    case srtp_err_status_no_such_op:
      return "no_such_op";
    case srtp_err_status_no_ctx:
      return "no_ctx";
    case srtp_err_status_cant_check:
      return "cant_check";
    case srtp_err_status_key_expired:
      return "key_expired";
    case srtp_err_status_socket_err:
      return "socket_err";
    case srtp_err_status_signal_err:
      return "signal_err";
    case srtp_err_status_nonce_bad:
      return "nonce_bad";
    case srtp_err_status_read_fail:
      return "read_fail";
    case srtp_err_status_write_fail:
      return "write_fail";
    case srtp_err_status_parse_err:
      return "parse_err";
    case srtp_err_status_encode_err:
      return "encode_err";
    case srtp_err_status_semaphore_err:
      return "semaphore_err";
    case srtp_err_status_pfkey_err:
      return "pfkey_err";
    default:
      return "unknown";
  }
}

srtp_err_status_t sfu_srtp_protect_rtp_status(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap) {
  if ((size_t)*len + SRTP_MAX_TRAILER_LEN > cap) {
    return srtp_err_status_bad_param;
  }
  return srtp_protect(ctx->outbound, buf, len);
}

bool sfu_srtp_protect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap) {
  if ((size_t)*len + SRTP_MAX_TRAILER_LEN > cap) {
    SFU_LOG_WARN("SRTP protect (RTP): insufficient buffer headroom (%d + trailer > %zu)", *len, cap);
    return false;
  }
  srtp_err_status_t rc = sfu_srtp_protect_rtp_status(ctx, buf, len, cap);
  if (rc != srtp_err_status_ok) {
    SFU_LOG_WARN("SRTP protect (RTP) failed: %d (%s)", (int)rc, sfu_srtp_status_name(rc));
    return false;
  }
  return true;
}

bool sfu_srtp_unprotect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len) {
  srtp_err_status_t rc = srtp_unprotect_rtcp(ctx->inbound, buf, len);
  if (rc != srtp_err_status_ok) {
    SFU_LOG_WARN("SRTP unprotect (RTCP) failed: %d", (int)rc);
    return false;
  }
  return true;
}

bool sfu_srtp_protect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap) {
  if (buf[1] == 206) {
    SFU_LOG_INFO("Received RTCP PLI (Keyframe Request)! Routing to the rest...");
  }

  if ((size_t)*len + SRTP_MAX_TRAILER_LEN + 4 > cap) { /* RTCP trailer also carries an E-flag+SRTCP index word */
    SFU_LOG_WARN("SRTP protect (RTCP): insufficient buffer headroom");
    return false;
  }
  srtp_err_status_t rc = srtp_protect_rtcp(ctx->outbound, buf, len);
  if (rc != srtp_err_status_ok) {
    SFU_LOG_WARN("SRTP protect (RTCP) failed: %d", (int)rc);
    return false;
  }
  return true;
}

bool sfu_rtp_is_rtcp(const uint8_t *data, size_t len) {
  if (len < 2) {
    return false;
  }
  uint8_t pt = data[1];
  return pt >= 192 && pt <= 223;
}
