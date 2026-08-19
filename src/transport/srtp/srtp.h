#ifndef SFU_TRANSPORT_SRTP_H
#define SFU_TRANSPORT_SRTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

int sfu_srtp_global_init(void);
void sfu_srtp_global_deinit(void);

int sfu_srtp_ctx_init_from_dtls(sfu_srtp_ctx_t *ctx, const uint8_t *keying_material, unsigned long profile_id, bool is_server);
void sfu_srtp_ctx_destroy(sfu_srtp_ctx_t *ctx);
srtp_err_status_t sfu_srtp_unprotect_rtp_status(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len);
bool sfu_srtp_unprotect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len);
srtp_err_status_t sfu_srtp_protect_rtp_status(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap);
bool sfu_srtp_protect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap);
const char *sfu_srtp_status_name(srtp_err_status_t status);
srtp_err_status_t sfu_srtp_unprotect_rtcp_status(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len);
bool sfu_srtp_unprotect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len);
bool sfu_srtp_protect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap);
bool sfu_rtp_is_rtcp(const uint8_t *data, size_t len);

#endif /* SFU_TRANSPORT_SRTP_H */
