#include "transport/stun/stun.h"
#include "util/log.h"

#include <arpa/inet.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <string.h>

#define STUN_MAGIC_COOKIE 0x2112A442u
#define STUN_HEADER_LEN 20
#define STUN_TYPE_BINDING_REQ 0x0001
#define STUN_TYPE_BINDING_RESP 0x0101

#define ATTR_USERNAME 0x0006
#define ATTR_MESSAGE_INTEGRITY 0x0008
#define ATTR_XOR_MAPPED_ADDRESS 0x0020
#define ATTR_FINGERPRINT 0x8028

static const char kIceAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static void random_ice_string(char *out, size_t len) {
  uint8_t raw[64];
  RAND_bytes(raw, (int)len);
  for (size_t i = 0; i < len; i++) {
    out[i] = kIceAlphabet[raw[i] % (sizeof(kIceAlphabet) - 1)];
  }
  out[len] = '\0';
}

void sfu_ice_credentials_generate(sfu_ice_credentials_t *out) {
  random_ice_string(out->ufrag, 8); /* RFC 8445: ufrag >= 4 chars */
  random_ice_string(out->pwd, 24);  /* RFC 8445: pwd >= 22 chars  */
}

bool sfu_stun_is_stun_packet(const uint8_t *data, size_t len) {
  if (len < STUN_HEADER_LEN)
    return false;
  /* Top two bits of the first byte must be 00 (RFC 7983 demux rule),
   * and the magic cookie must match -- cheap, effective filter against
   * DTLS/SRTP packets which never satisfy both. */
  if ((data[0] & 0xC0) != 0x00)
    return false;
  uint32_t cookie = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                    ((uint32_t)data[6] << 8) | (uint32_t)data[7];
  return cookie == STUN_MAGIC_COOKIE;
}

static void write_be16(uint8_t *p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v & 0xFF;
}
static void write_be32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static uint16_t read_be16(const uint8_t *p) {
  return ((uint16_t)p[0] << 8) | p[1];
}

/* Standard bitwise CRC-32 (IEEE 802.3 polynomial). STUN FINGERPRINT is
 * only ever computed on a handful of ~100-byte messages, so the
 * unrolled table-based version isn't worth the code -- this isn't a
 * hot path. */
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc >>= 1;
      }
    }
  }
  return ~crc;
}

/* Locates the MESSAGE-INTEGRITY attribute (if present) and returns the
 * byte offset of its TLV header. Returns 0 (never a valid offset, since
 * that's inside the STUN header) if not found. */
static size_t find_message_integrity(const uint8_t *data, size_t len) {
  size_t off = STUN_HEADER_LEN;
  while (off + 4 <= len) {
    uint16_t attr_type = read_be16(data + off);
    uint16_t attr_len = read_be16(data + off + 2);
    size_t padded = (attr_len + 3) & ~((size_t)3);
    if (attr_type == ATTR_MESSAGE_INTEGRITY) {
      return off;
    }
    off += 4 + padded;
  }
  return 0;
}

static const uint8_t *find_username(const uint8_t *data, size_t len,
                                    uint16_t *out_len) {
  size_t off = STUN_HEADER_LEN;
  while (off + 4 <= len) {
    uint16_t attr_type = read_be16(data + off);
    uint16_t attr_len = read_be16(data + off + 2);
    size_t padded = (attr_len + 3) & ~((size_t)3);
    if (attr_type == ATTR_USERNAME && off + 4 + attr_len <= len) {
      *out_len = attr_len;
      return data + off + 4;
    }
    off += 4 + padded;
  }
  return NULL;
}

static bool verify_message_integrity(const uint8_t *data, size_t len,
                                     size_t mi_offset, const char *pwd) {
  if (mi_offset == 0 || mi_offset + 24 > len)
    return false; /* 4 hdr + 20 value */

  /* HMAC covers bytes [0, mi_offset) with the STUN header's length
   * field temporarily patched to the length "as if" the message ended
   * right after this M-I attribute (RFC 5389 15.4). */
  uint8_t scratch[1500];
  if (mi_offset > sizeof(scratch))
    return false;
  memcpy(scratch, data, mi_offset);
  uint16_t patched_len = (uint16_t)((mi_offset - STUN_HEADER_LEN) + 24);
  write_be16(scratch + 2, patched_len);

  uint8_t computed[20];
  unsigned int computed_len = 0;
  HMAC(EVP_sha1(), pwd, (int)strlen(pwd), scratch, mi_offset, computed,
       &computed_len);
  if (computed_len != 20)
    return false;

  return memcmp(computed, data + mi_offset + 4, 20) == 0;
}

/* Appends MESSAGE-INTEGRITY, then FINGERPRINT, to a response buffer
 * that already has its header + XOR-MAPPED-ADDRESS written. Patches the
 * header length field twice, once for each attribute, per RFC 5389. */
static size_t append_integrity_and_fingerprint(uint8_t *buf, size_t body_len,
                                               const char *pwd) {
  /* MESSAGE-INTEGRITY: header length must cover up through this attr. */
  write_be16(buf + 2, (uint16_t)((body_len - STUN_HEADER_LEN) + 24));

  uint8_t hmac[20];
  unsigned int hmac_len = 0;
  HMAC(EVP_sha1(), pwd, (int)strlen(pwd), buf, body_len, hmac, &hmac_len);

  write_be16(buf + body_len, ATTR_MESSAGE_INTEGRITY);
  write_be16(buf + body_len + 2, 20);
  memcpy(buf + body_len + 4, hmac, 20);
  body_len += 4 + 20;

  /* FINGERPRINT: header length now covers up through this attr too. */
  write_be16(buf + 2, (uint16_t)((body_len - STUN_HEADER_LEN) + 8));
  uint32_t crc = crc32_ieee(buf, body_len) ^ 0x5354554Eu;
  write_be16(buf + body_len, ATTR_FINGERPRINT);
  write_be16(buf + body_len + 2, 4);
  write_be32(buf + body_len + 4, crc);
  body_len += 4 + 4;

  return body_len;
}

size_t sfu_stun_handle_binding_request(const uint8_t *data, size_t len,
                                       const sfu_ice_credentials_t *local,
                                       const struct sockaddr_storage *src_addr,
                                       socklen_t src_addr_len, uint8_t *out_buf,
                                       size_t out_buf_cap) {
  if (!sfu_stun_is_stun_packet(data, len))
    return 0;

  uint16_t msg_type = read_be16(data);
  if (msg_type != STUN_TYPE_BINDING_REQ)
    return 0; /* only handle requests */

  uint16_t username_len = 0;
  const uint8_t *username = find_username(data, len, &username_len);
  if (!username)
    return 0;

  /* USERNAME must be "{our-ufrag}:{their-ufrag}" -- verify the prefix
   * matches our local ufrag exactly (see file header on credentials). */
  size_t ufrag_len = strlen(local->ufrag);
  if (username_len <= ufrag_len || username[ufrag_len] != ':' ||
      memcmp(username, local->ufrag, ufrag_len) != 0) {
    return 0;
  }

  size_t mi_offset = find_message_integrity(data, len);
  if (!verify_message_integrity(data, len, mi_offset, local->pwd)) {
    SFU_LOG_WARN(
        "STUN binding request failed MESSAGE-INTEGRITY check, dropping");
    return 0;
  }

  if (src_addr->ss_family != AF_INET ||
      out_buf_cap < STUN_HEADER_LEN + 12 + 24 + 8) {
    return 0; /* IPv6 XOR-MAPPED-ADDRESS and undersized buffers unsupported here
               */
  }

  /* --- Build the Binding Success Response --- */
  write_be16(out_buf, STUN_TYPE_BINDING_RESP);
  write_be16(out_buf + 2, 0); /* patched below once attributes are known */
  write_be32(out_buf + 4, STUN_MAGIC_COOKIE);
  memcpy(out_buf + 8, data + 8, 12); /* echo the transaction ID */

  size_t off = STUN_HEADER_LEN;

  /* XOR-MAPPED-ADDRESS (IPv4) */
  const struct sockaddr_in *sin = (const struct sockaddr_in *)src_addr;
  (void)src_addr_len;
  write_be16(out_buf + off, ATTR_XOR_MAPPED_ADDRESS);
  write_be16(out_buf + off + 2, 8);
  out_buf[off + 4] = 0x00;
  out_buf[off + 5] = 0x01; /* family: IPv4 */
  uint16_t xport = ntohs(sin->sin_port) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
  write_be16(out_buf + off + 6, xport);
  uint32_t xaddr = ntohl(sin->sin_addr.s_addr) ^ STUN_MAGIC_COOKIE;
  write_be32(out_buf + off + 8, xaddr);
  off += 4 + 8;

  off = append_integrity_and_fingerprint(out_buf, off, local->pwd);

  return off;
}
