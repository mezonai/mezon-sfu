#ifndef SFU_TRANSPORT_DTLS_H
#define SFU_TRANSPORT_DTLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

/* Generates a self-signed EC (P-256) certificate and key, builds an
 * SSL_CTX configured for DTLS 1.2 server operation with the
 * SRTP_AES128_CM_SHA1_80 profile offered. Returns 0 on success. */
int sfu_dtls_ctx_init(sfu_dtls_ctx_t *ctx);
void sfu_dtls_ctx_destroy(sfu_dtls_ctx_t *ctx);

/* Sets up one server-side DTLS connection (accept state) against a
 * shared ctx. One of these lives per peer session (see peer/session.h). */
int sfu_dtls_conn_init(sfu_dtls_conn_t *conn, sfu_dtls_ctx_t *ctx);
void sfu_dtls_conn_destroy(sfu_dtls_conn_t *conn);

/* Feeds one received datagram into the handshake state machine. */
sfu_dtls_feed_status_t sfu_dtls_conn_feed(sfu_dtls_conn_t *conn, const uint8_t *data, size_t len, void (*on_established_cb)(void *userdata), void *userdata);

/* Drains any handshake bytes OpenSSL wants sent back to the peer after
 * a feed call (or after conn_init, for the very first flight if we were
 * the ones to initiate -- not the case here, we're always the DTLS
 * server and wait for the client's ClientHello first). Returns the
 * number of bytes written into out (0 if nothing pending). */
size_t sfu_dtls_conn_drain_output(sfu_dtls_conn_t *conn, uint8_t *out, size_t cap);

/* RFC 7983 demux helper: bytes 20-63 in the first octet identify DTLS,
 * distinct from STUN (top 2 bits 00) and RTP/RTCP (top 2 bits 1x). */
bool sfu_dtls_is_dtls_packet(const uint8_t *data, size_t len);

#endif /* SFU_TRANSPORT_DTLS_H */
