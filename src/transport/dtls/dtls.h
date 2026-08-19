#ifndef SFU_TRANSPORT_DTLS_H
#define SFU_TRANSPORT_DTLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sfu/datadef.h"

int sfu_dtls_ctx_init(sfu_dtls_ctx_t *ctx);
void sfu_dtls_ctx_destroy(sfu_dtls_ctx_t *ctx);
int sfu_dtls_conn_init(sfu_dtls_conn_t *conn, sfu_dtls_ctx_t *ctx);
void sfu_dtls_conn_destroy(sfu_dtls_conn_t *conn);
sfu_dtls_feed_status_t sfu_dtls_conn_feed(sfu_dtls_conn_t *conn, const uint8_t *data, size_t len, void (*on_established_cb)(void *userdata), void *userdata);
size_t sfu_dtls_conn_drain_output(sfu_dtls_conn_t *conn, uint8_t *out, size_t cap);
bool sfu_dtls_is_dtls_packet(const uint8_t *data, size_t len);
bool sfu_dtls_extract_client_hello_random(const uint8_t *data, size_t len, uint8_t random_out[32]);

#endif /* SFU_TRANSPORT_DTLS_H */
