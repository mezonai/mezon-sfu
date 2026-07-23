#ifndef SFU_TRANSPORT_SRTP_H
#define SFU_TRANSPORT_SRTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

/* Call once at process startup/shutdown (libsrtp2 keeps global state). */
int sfu_srtp_global_init(void);
void sfu_srtp_global_deinit(void);

/* Derives both directions' keys from a completed DTLS-SRTP handshake's
 * exported keying material and creates the two libsrtp sessions. Uses
 * SSRC-wildcard policies (ssrc_any_inbound/outbound) since there's no
 * per-SSRC tracking yet (rtp/parser.c doesn't exist). Returns 0 on
 * success. */
int sfu_srtp_ctx_init_from_dtls(sfu_srtp_ctx_t *ctx, const uint8_t *keying_material, unsigned long profile_id, bool is_server);
void sfu_srtp_ctx_destroy(sfu_srtp_ctx_t *ctx);

/*
 * In-place unprotect/protect. `len` is read as the ciphertext length on
 * entry to unprotect (plaintext length on entry to protect) and
 * overwritten with the resulting length. Buffers passed to protect()
 * must have at least SRTP_MAX_TRAILER_LEN bytes of spare capacity past
 * `*len` for the appended auth tag -- callers allocate accordingly.
 * Returns false on any crypto/auth failure (caller must drop the
 * packet, never forward on a failed unprotect).
 */
bool sfu_srtp_unprotect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len);
bool sfu_srtp_protect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap);
bool sfu_srtp_unprotect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len);
bool sfu_srtp_protect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len, size_t cap);

/* RFC 7983-adjacent split within the RTP/RTCP first-byte range
 * (128-191): RTCP packet types occupy 192-223 in the *second* byte
 * (SR=200, RR=201, SDES=202, BYE=203, APP=204, and the extended range
 * used by RTPFB/PSFB/XR). Everything else in range is RTP. */
bool sfu_rtp_is_rtcp(const uint8_t *data, size_t len);

#endif /* SFU_TRANSPORT_SRTP_H */
