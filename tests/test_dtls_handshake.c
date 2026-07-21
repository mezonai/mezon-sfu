#include "transport/dtls/dtls.h"

#include <assert.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  sfu_dtls_ctx_t server_ctx;
  assert(sfu_dtls_ctx_init(&server_ctx) == 0);
  assert(strlen(server_ctx.fingerprint) > 0);

  sfu_dtls_conn_t server_conn;
  assert(sfu_dtls_conn_init(&server_conn, &server_ctx) == 0);

  /* Real OpenSSL DTLS client, driven the same way a browser's DTLS
   * stack would be: memory BIOs, no cert verification (WebRTC pins by
   * SDP fingerprint instead), same SRTP profile offered. This proves
   * sfu_dtls_conn_t actually interoperates with a standards-compliant
   * DTLS implementation, not just itself. */
  SSL_CTX *client_ctx = SSL_CTX_new(DTLS_client_method());
  assert(client_ctx != NULL);
  SSL_CTX_set_min_proto_version(client_ctx, DTLS1_2_VERSION);
  SSL_CTX_set_max_proto_version(client_ctx, DTLS1_2_VERSION);
  SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, NULL);
  assert(SSL_CTX_set_tlsext_use_srtp(client_ctx, "SRTP_AES128_CM_SHA1_80") == 0);

  SSL *client_ssl = SSL_new(client_ctx);
  BIO *client_rbio = BIO_new(BIO_s_mem());
  BIO *client_wbio = BIO_new(BIO_s_mem());
  BIO_set_mem_eof_return(client_rbio, -1);
  SSL_set_bio(client_ssl, client_rbio, client_wbio);
  SSL_set_connect_state(client_ssl);

  bool client_done = false, server_done = false;
  uint8_t buf[4096];

  int rc = SSL_do_handshake(client_ssl);
  if (rc == 1) {
    client_done = true;
  }

  int iterations = 0;
  while (!(client_done && server_done) && iterations++ < 50) {
    int n;
    bool progressed = false;
    while ((n = BIO_read(client_wbio, buf, sizeof(buf))) > 0) {
      progressed = true;

      sfu_dtls_feed_status_t st = sfu_dtls_conn_feed(&server_conn, buf, (size_t)n, NULL, NULL);
      assert(st != SFU_DTLS_FEED_ERROR);
      if (st == SFU_DTLS_FEED_ESTABLISHED) {
        server_done = true;
      }

      uint8_t out[4096];
      size_t out_len = sfu_dtls_conn_drain_output(&server_conn, out, sizeof(out));
      if (out_len > 0) {
        BIO_write(client_rbio, out, (int)out_len);
      }
    }

    if (!client_done) {
      int crc = SSL_do_handshake(client_ssl);
      if (crc == 1) {
        client_done = true;
        progressed = true;
      } else {
        int err = SSL_get_error(client_ssl, crc);
        assert(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE);
      }
    }

    if (!progressed && !client_done) {
      /* Neither side produced anything this round; avoid spinning
       * forever on a stuck handshake -- treat as failure. */
      break;
    }
  }

  assert(client_done);
  assert(server_done);
  assert(server_conn.established);

  /* Both sides must derive byte-identical SRTP keying material from
   * the same DTLS master secret -- this is the actual point of DTLS-
   * SRTP (RFC 5764), not just "the handshake completed". */
  uint8_t client_material[SFU_SRTP_KEY_MATERIAL_LEN];
  assert(SSL_export_keying_material(client_ssl, client_material, SFU_SRTP_KEY_MATERIAL_LEN, "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0) == 1);
  assert(memcmp(client_material, server_conn.srtp_keying_material, SFU_SRTP_KEY_MATERIAL_LEN) == 0);

  /* Both sides must have agreed on the same SRTP protection profile. */
  const SRTP_PROTECTION_PROFILE *client_profile = SSL_get_selected_srtp_profile(client_ssl);
  assert(client_profile != NULL);
  assert(strcmp(client_profile->name, "SRTP_AES128_CM_SHA1_80") == 0);

  SSL_free(client_ssl);
  SSL_CTX_free(client_ctx);
  sfu_dtls_conn_destroy(&server_conn);
  sfu_dtls_ctx_destroy(&server_ctx);

  printf("test_dtls_handshake: OK\n");
  return 0;
}
