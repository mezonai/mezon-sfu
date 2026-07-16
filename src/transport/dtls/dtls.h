#ifndef SFU_TRANSPORT_DTLS_H
#define SFU_TRANSPORT_DTLS_H

#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * DTLS-SRTP (RFC 5764) server side, driven by our own io_uring UDP
 * socket rather than OpenSSL's socket BIO -- we feed received datagrams
 * in via a memory BIO and drain OpenSSL's desired output via another,
 * so the handshake bytes travel over the exact same send_zc path as
 * everything else instead of OpenSSL opening its own socket.
 *
 * WebRTC doesn't use CA-verified certificates: the self-signed cert
 * generated here is authenticated out of band via its SHA-256
 * fingerprint in the SDP answer (a=fingerprint), not a trust chain --
 * so certificate verification is intentionally off (SSL_VERIFY_NONE).
 * That fingerprint isn't wired up yet since protocol/signaling/ doesn't
 * exist -- see sfu_dtls_ctx_t's fingerprint field, computed but unused
 * until SDP generation lands.
 */

#define SFU_SRTP_KEY_MATERIAL_LEN                                              \
  60 /* SRTP_AES128_CM_SHA1_80: 2 x (16-byte key + 14-byte salt) */
#define SFU_DTLS_FINGERPRINT_LEN                                               \
  96 /* "XX:XX:...:XX\0" for SHA-256, 32 bytes -> 95 chars + nul */

typedef struct sfu_dtls_ctx {
  SSL_CTX *ssl_ctx;
  char fingerprint[SFU_DTLS_FINGERPRINT_LEN]; /* certificate SHA-256, colon-hex
                                               */
} sfu_dtls_ctx_t;

/* Generates a self-signed EC (P-256) certificate and key, builds an
 * SSL_CTX configured for DTLS 1.2 server operation with the
 * SRTP_AES128_CM_SHA1_80 profile offered. Returns 0 on success. */
int sfu_dtls_ctx_init(sfu_dtls_ctx_t *ctx);
void sfu_dtls_ctx_destroy(sfu_dtls_ctx_t *ctx);

typedef struct sfu_dtls_conn {
  SSL *ssl;
  BIO *rbio; /* received datagrams get written here before SSL_do_handshake */
  BIO *wbio; /* OpenSSL writes its desired output here for us to drain+send */
  bool established;
  uint8_t srtp_keying_material[SFU_SRTP_KEY_MATERIAL_LEN];
} sfu_dtls_conn_t;

/* Sets up one server-side DTLS connection (accept state) against a
 * shared ctx. One of these lives per peer session (see peer/session.h). */
int sfu_dtls_conn_init(sfu_dtls_conn_t *conn, sfu_dtls_ctx_t *ctx);
void sfu_dtls_conn_destroy(sfu_dtls_conn_t *conn);

typedef enum {
  SFU_DTLS_FEED_ERROR = -1, /* fatal: drop this connection/session */
  SFU_DTLS_FEED_IN_PROGRESS =
      0, /* handshake continuing; drain output and send it */
  SFU_DTLS_FEED_ESTABLISHED =
      1, /* handshake complete; srtp_keying_material is valid */
} sfu_dtls_feed_status_t;

/* Feeds one received datagram into the handshake state machine. */
sfu_dtls_feed_status_t sfu_dtls_conn_feed(sfu_dtls_conn_t *conn,
                                          const uint8_t *data, size_t len);

/* Drains any handshake bytes OpenSSL wants sent back to the peer after
 * a feed call (or after conn_init, for the very first flight if we were
 * the ones to initiate -- not the case here, we're always the DTLS
 * server and wait for the client's ClientHello first). Returns the
 * number of bytes written into out (0 if nothing pending). */
size_t sfu_dtls_conn_drain_output(sfu_dtls_conn_t *conn, uint8_t *out,
                                  size_t cap);

/* RFC 7983 demux helper: bytes 20-63 in the first octet identify DTLS,
 * distinct from STUN (top 2 bits 00) and RTP/RTCP (top 2 bits 1x). */
bool sfu_dtls_is_dtls_packet(const uint8_t *data, size_t len);

#endif /* SFU_TRANSPORT_DTLS_H */
