#ifndef SFU_TRANSPORT_SRTP_H
#define SFU_TRANSPORT_SRTP_H

#include <srtp2/srtp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wraps libsrtp2 around the keying material DTLS-SRTP hands us
 * (transport/dtls/dtls.h's SFU_SRTP_KEY_MATERIAL_LEN=60 bytes). Per
 * RFC 5764, the exported material is laid out as:
 *   [0:16)  client_write_master_key    [16:32) server_write_master_key
 *   [32:46) client_write_master_salt   [46:60) server_write_master_salt
 *
 * We are always the DTLS *server* in this handshake, so:
 *   - inbound  (decrypting what the peer sends us)  uses client_write
 *   - outbound (encrypting what we send the peer)    uses server_write
 *
 * Each peer session owns exactly one sfu_srtp_ctx_t -- see
 * peer/session.h. A real SFU is necessarily a decrypt-then-re-encrypt
 * relay, not a blind byte forwarder: every peer negotiated independent
 * DTLS keys, so a publisher's ciphertext must be decrypted once with
 * *its* session's inbound key, then re-encrypted once per subscriber
 * with *that subscriber's* own outbound key before forwarding. See
 * runtime/worker.c's sfu_room_forward_packet for where that happens.
 */
typedef struct sfu_srtp_ctx {
  srtp_t inbound;  /* decrypts packets FROM this peer */
  srtp_t outbound; /* encrypts packets TO this peer   */
} sfu_srtp_ctx_t;

/* Call once at process startup/shutdown (libsrtp2 keeps global state). */
int sfu_srtp_global_init(void);
void sfu_srtp_global_deinit(void);

/* Derives both directions' keys from a completed DTLS-SRTP handshake's
 * exported keying material and creates the two libsrtp sessions. Uses
 * SSRC-wildcard policies (ssrc_any_inbound/outbound) since there's no
 * per-SSRC tracking yet (rtp/parser.c doesn't exist). Returns 0 on
 * success. */
int sfu_srtp_ctx_init_from_dtls(sfu_srtp_ctx_t *ctx,
                                const uint8_t keying_material[60]);
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
bool sfu_srtp_protect_rtp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len,
                          size_t cap);
bool sfu_srtp_unprotect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len);
bool sfu_srtp_protect_rtcp(sfu_srtp_ctx_t *ctx, uint8_t *buf, int *len,
                           size_t cap);

/* RFC 7983-adjacent split within the RTP/RTCP first-byte range
 * (128-191): RTCP packet types occupy 192-223 in the *second* byte
 * (SR=200, RR=201, SDES=202, BYE=203, APP=204, and the extended range
 * used by RTPFB/PSFB/XR). Everything else in range is RTP. */
bool sfu_rtp_is_rtcp(const uint8_t *data, size_t len);

#endif /* SFU_TRANSPORT_SRTP_H */
