#include "transport/dtls/dtls.h"
#include "util/log.h"

#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>
#ifdef OPENSSL_IS_BORINGSSL
#include <openssl/ec_key.h>
#endif

static int generate_self_signed_cert(EVP_PKEY **out_pkey, X509 **out_cert) {
  EC_KEY *ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  if (!ec_key) {
    return -1;
  }
  if (!EC_KEY_generate_key(ec_key)) {
    EC_KEY_free(ec_key);
    return -1;
  }

  EVP_PKEY *pkey = EVP_PKEY_new();
  if (!pkey || !EVP_PKEY_assign_EC_KEY(pkey, ec_key)) {
    EC_KEY_free(ec_key);
    if (pkey) {
      EVP_PKEY_free(pkey);
    }
    return -1;
  }

  X509 *cert = X509_new();
  if (!cert) {
    EVP_PKEY_free(pkey);
    return -1;
  }

  ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(cert), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 365);

  X509_NAME *name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"mezon-sfu", -1, -1, 0);
  X509_set_issuer_name(cert, name);

  X509_set_pubkey(cert, pkey);
  if (!X509_sign(cert, pkey, EVP_sha256())) {
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return -1;
  }

  *out_pkey = pkey;
  *out_cert = cert;
  return 0;
}

static void compute_fingerprint(X509 *cert, char *out, size_t out_cap) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  X509_digest(cert, EVP_sha256(), digest, &digest_len);

  size_t pos = 0;
  for (unsigned int i = 0; i < digest_len && pos + 3 < out_cap; i++) {
    pos += (size_t)snprintf(out + pos, out_cap - pos, "%s%02X", i ? ":" : "", digest[i]);
  }
}

int sfu_dtls_ctx_init(sfu_dtls_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));

  EVP_PKEY *pkey = NULL;
  X509 *cert = NULL;
  if (generate_self_signed_cert(&pkey, &cert) != 0) {
    SFU_LOG_ERROR("DTLS: failed to generate self-signed certificate");
    return -1;
  }

  compute_fingerprint(cert, ctx->fingerprint, sizeof(ctx->fingerprint));

  ctx->ssl_ctx = SSL_CTX_new(DTLS_server_method());
  if (!ctx->ssl_ctx) {
    SFU_LOG_ERROR("DTLS: SSL_CTX_new failed");
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return -1;
  }

  SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);
  SSL_CTX_set_max_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);

  SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
  SSL_CTX_set_cipher_list(ctx->ssl_ctx, "DEFAULT:!aNULL:!eNULL");

  if (SSL_CTX_use_certificate(ctx->ssl_ctx, cert) != 1 || SSL_CTX_use_PrivateKey(ctx->ssl_ctx, pkey) != 1) {
    SFU_LOG_ERROR("DTLS: failed to install certificate/key into SSL_CTX");
    X509_free(cert);
    EVP_PKEY_free(pkey);
    SSL_CTX_free(ctx->ssl_ctx);
    ctx->ssl_ctx = NULL;
    return -1;
  }

  X509_free(cert);
  EVP_PKEY_free(pkey);

  if (SSL_CTX_set_tlsext_use_srtp(ctx->ssl_ctx,
                                  "SRTP_AEAD_AES_128_GCM:SRTP_AEAD_AES_256_GCM:"
                                  "SRTP_AES128_CM_SHA1_80") != 0) {
    SFU_LOG_ERROR("DTLS: failed to offer SRTP profiles");
    SSL_CTX_free(ctx->ssl_ctx);
    ctx->ssl_ctx = NULL;
    return -1;
  }

  SFU_LOG_INFO("DTLS context ready, cert fingerprint sha-256 %s", ctx->fingerprint);
  return 0;
}

void sfu_dtls_ctx_destroy(sfu_dtls_ctx_t *ctx) {
  if (ctx->ssl_ctx) {
    SSL_CTX_free(ctx->ssl_ctx);
    ctx->ssl_ctx = NULL;
  }
}

int sfu_dtls_conn_init(sfu_dtls_conn_t *conn, sfu_dtls_ctx_t *ctx) {
  memset(conn, 0, sizeof(*conn));

  conn->ssl = SSL_new(ctx->ssl_ctx);
  if (!conn->ssl) {
    return -1;
  }

  conn->rbio = BIO_new(BIO_s_mem());
  conn->wbio = BIO_new(BIO_s_mem());
  if (!conn->rbio || !conn->wbio) {
    BIO_free(conn->rbio);
    BIO_free(conn->wbio);
    conn->rbio = NULL;
    conn->wbio = NULL;
    SSL_free(conn->ssl);
    conn->ssl = NULL;
    return -1;
  }

  BIO_set_mem_eof_return(conn->rbio, -1);

  SSL_set_bio(conn->ssl, conn->rbio, conn->wbio);
  SSL_set_accept_state(conn->ssl);

  return 0;
}

void sfu_dtls_conn_destroy(sfu_dtls_conn_t *conn) {
  if (conn->ssl) {
    SSL_free(conn->ssl);
    conn->ssl = NULL;
    conn->rbio = NULL;
    conn->wbio = NULL;
  }
}

sfu_dtls_feed_status_t sfu_dtls_conn_feed(sfu_dtls_conn_t *conn, const uint8_t *data, size_t len, void (*on_established_cb)(void *userdata), void *userdata) {
  if (conn->established) {
    return SFU_DTLS_FEED_ESTABLISHED;
  }

  BIO_write(conn->rbio, data, (int)len);

  int rc = SSL_do_handshake(conn->ssl);
  if (rc == 1) {
    const SRTP_PROTECTION_PROFILE *profile = SSL_get_selected_srtp_profile(conn->ssl);
    if (!profile) {
      SFU_LOG_ERROR("DTLS: handshake succeeded, but no SRTP profile negotiated!");
      return SFU_DTLS_FEED_ERROR;
    }

    conn->srtp_profile_id = profile->id;

    size_t material_len = 60;  // Default for SRTP_AES128_CM_SHA1_80 (0x0001)
    if (profile->id == 0x0007) {
      material_len = 56;  // GCM-128 (16 * 2 + 12 * 2)
    } else if (profile->id == 0x0008) {
      material_len = 88;  // GCM-256 (32 * 2 + 12 * 2)
    }
    if (material_len > sizeof(conn->srtp_keying_material)) {
      SFU_LOG_ERROR("DTLS: SRTP keying material length %zu exceeds buffer %zu (profile 0x%04lx)", material_len, sizeof(conn->srtp_keying_material),
                    profile->id);
      return SFU_DTLS_FEED_ERROR;
    }

    if (SSL_export_keying_material(conn->ssl, conn->srtp_keying_material, material_len, "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0) != 1) {
      SFU_LOG_ERROR("DTLS: handshake completed but SRTP key export failed");
      return SFU_DTLS_FEED_ERROR;
    }

    conn->established = true;
    SFU_LOG_INFO("DTLS handshake established. Profile: %s (0x%04lx)", profile->name, profile->id);

    // Fire the signaling renegotiation event ONLY now that media pathways are secure
    if (on_established_cb) {
      on_established_cb(userdata);
    }

    return SFU_DTLS_FEED_ESTABLISHED;
  }

  int err = SSL_get_error(conn->ssl, rc);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    return SFU_DTLS_FEED_IN_PROGRESS;
  }

  SFU_LOG_WARN("DTLS handshake error: %s", ERR_reason_error_string(ERR_get_error()));
  return SFU_DTLS_FEED_ERROR;
}

size_t sfu_dtls_conn_drain_output(sfu_dtls_conn_t *conn, uint8_t *out, size_t cap) {
  size_t total = 0;
  while (total < cap) {
    int rc = BIO_read(conn->wbio, out + total, (int)(cap - total));
    if (rc <= 0) {
      break;
    }
    total += (size_t)rc;
  }
  return total;
}

bool sfu_dtls_is_dtls_packet(const uint8_t *data, size_t len) {
  if (len < 1) {
    return false;
  }
  return data[0] >= 20 && data[0] <= 63; /* RFC 7983 */
}
