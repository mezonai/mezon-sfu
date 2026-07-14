#include "transport/srtp/srtp.h"
#include "util/log.h"

#include <string.h>

#define SFU_SRTP_MASTER_KEY_LEN  16
#define SFU_SRTP_MASTER_SALT_LEN 14

int sfu_srtp_global_init(void) {
    srtp_err_status_t rc = srtp_init();
    if (rc != srtp_err_status_ok) {
        SFU_LOG_ERROR("srtp_init failed: %d", (int)rc);
        return -1;
    }
    return 0;
}

void sfu_srtp_global_deinit(void) {
    srtp_shutdown();
}

/* Concatenates a 16-byte key + 14-byte salt (libsrtp's expected master
 * key buffer layout for AES_CM_128_HMAC_SHA1_80) into `out[30]`. */
static void build_master_key(const uint8_t *key16, const uint8_t *salt14, uint8_t out[30]) {
    memcpy(out, key16, SFU_SRTP_MASTER_KEY_LEN);
    memcpy(out + SFU_SRTP_MASTER_KEY_LEN, salt14, SFU_SRTP_MASTER_SALT_LEN);
}

static int create_session(srtp_t *session, const uint8_t master_key[30], srtp_ssrc_type_t ssrc_type) {
    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));

    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    policy.ssrc.type = ssrc_type;
    policy.ssrc.value = 0; /* ignored for wildcard types */
    policy.key = (uint8_t *)master_key; /* srtp_create copies it internally */
    policy.next = NULL;
    policy.window_size = 128;
    policy.allow_repeat_tx = 0;

    srtp_err_status_t rc = srtp_create(session, &policy);
    if (rc != srtp_err_status_ok) {
        SFU_LOG_ERROR("srtp_create failed: %d", (int)rc);
        return -1;
    }
    return 0;
}

int sfu_srtp_ctx_init_from_dtls(sfu_srtp_ctx_t *ctx, const uint8_t keying_material[60]) {
    memset(ctx, 0, sizeof(*ctx));

    const uint8_t *client_key  = keying_material;
    const uint8_t *server_key  = keying_material + 16;
    const uint8_t *client_salt = keying_material + 32;
    const uint8_t *server_salt = keying_material + 46;

    uint8_t client_master[30], server_master[30];
    build_master_key(client_key, client_salt, client_master);
    build_master_key(server_key, server_salt, server_master);

    /* We are the DTLS server: inbound (decrypting the peer's traffic)
     * uses the client's key, outbound (our traffic to the peer) uses
     * the server's key -- see the header comment for why. */
    int rc1 = create_session(&ctx->inbound, client_master, ssrc_any_inbound);
    int rc2 = create_session(&ctx->outbound, server_master, ssrc_any_outbound);

    /* Zero the local copies regardless of outcome -- these are session
     * keys, no reason to leave them sitting in a stack frame. */
    memset(client_master, 0, sizeof(client_master));
    memset(server_master, 0, sizeof(server_master));

    if (rc1 != 0 || rc2 != 0) {
        if (ctx->inbound) srtp_dealloc(ctx->inbound);
        if (ctx->outbound) srtp_dealloc(ctx->outbound);
        memset(ctx, 0, sizeof(*ctx));
        return -1;
    }

    return 0;
}

void sfu_srtp_ctx_destroy(sfu_srtp_ctx_t *ctx) {
    if (ctx->inbound) { srtp_dealloc(ctx->inbound); ctx->inbound = NULL; }
    if (ctx->outbound) { srtp_dealloc(ctx->outbound); ctx->outbound = NULL; }
}

bool sfu_srtp_unprotect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len) {
    srtp_err_status_t rc = srtp_unprotect(ctx->inbound, buf, len);
    if (rc != srtp_err_status_ok) {
        SFU_LOG_WARN("SRTP unprotect (RTP) failed: %d", (int)rc);
        return false;
    }
    return true;
}

bool sfu_srtp_protect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap) {
    if ((size_t)*len + SRTP_MAX_TRAILER_LEN > cap) {
        SFU_LOG_WARN("SRTP protect (RTP): insufficient buffer headroom (%d + trailer > %zu)",
                     *len, cap);
        return false;
    }
    srtp_err_status_t rc = srtp_protect(ctx->outbound, buf, len);
    if (rc != srtp_err_status_ok) {
        SFU_LOG_WARN("SRTP protect (RTP) failed: %d", (int)rc);
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
    if (len < 2) return false;
    uint8_t pt = data[1];
    return pt >= 192 && pt <= 223;
}
