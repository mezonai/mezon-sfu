/* Unit tests for the transport-wide CC RTP extension writer (CC-01). */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtp/rtp_ext.h"
#include "rtp/rtp_packet.h"
#include "util/netbytes.h"

#define TWCC_ID 5

static size_t build_rtp(uint8_t *buf, bool with_ext, uint16_t ext_profile, const uint8_t *ext_body, size_t ext_body_len, size_t payload_len) {
  buf[0] = 0x80 | (with_ext ? 0x10 : 0);
  buf[1] = 96;
  sfu_write_be16(buf + 2, 1234);
  sfu_write_be32(buf + 4, 0x11223344);
  sfu_write_be32(buf + 8, 0x0a0b0c0d);
  size_t off = 12;
  if (with_ext) {
    sfu_write_be16(buf + off, ext_profile);
    sfu_write_be16(buf + off + 2, (uint16_t)(ext_body_len / 4));
    memcpy(buf + off + 4, ext_body, ext_body_len);
    off += 4 + ext_body_len;
  }
  memset(buf + off, 0xab, payload_len);
  return off + payload_len;
}

/* Parses the result and returns the TWCC value via the strict parser's view
 * of the extension block. */
static bool read_twcc(const uint8_t *buf, size_t len, uint8_t ext_id, uint16_t *out) {
  sfu_rtp_packet_t p;
  if (!sfu_rtp_packet_parse(buf, len, &p) || !p.extension) {
    return false;
  }
  if (p.extension_profile == 0xBEDE) {
    size_t pos = 0;
    while (pos < p.extension_length) {
      uint8_t b = p.extension_data[pos];
      if (b == 0) {
        pos++;
        continue;
      }
      uint8_t id = b >> 4;
      size_t dl = (size_t)(b & 0x0f) + 1;
      if (id == 15 || pos + 1 + dl > p.extension_length) {
        return false;
      }
      if (id == ext_id && dl == 2) {
        *out = sfu_read_be16(p.extension_data + pos + 1);
        return true;
      }
      pos += 1 + dl;
    }
    return false;
  }
  if ((p.extension_profile & 0xFFF0) == 0x1000) {
    size_t pos = 0;
    while (pos < p.extension_length) {
      uint8_t id = p.extension_data[pos];
      if (id == 0) {
        pos++;
        continue;
      }
      if (pos + 2 > p.extension_length) {
        return false;
      }
      size_t dl = p.extension_data[pos + 1];
      if (pos + 2 + dl > p.extension_length) {
        return false;
      }
      if (id == ext_id && dl == 2) {
        *out = sfu_read_be16(p.extension_data + pos + 2);
        return true;
      }
      pos += 2 + dl;
    }
    return false;
  }
  return false;
}

/* No extension block: a new one-byte block is created, X bit set, payload
 * preserved. */
static void test_insert_into_plain_packet(void) {
  uint8_t buf[256];
  size_t len = build_rtp(buf, false, 0, NULL, 0, 20);
  uint8_t payload_copy[20];
  memcpy(payload_copy, buf + 12, 20);

  size_t out_len = 0;
  assert(sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 4242, &out_len));
  assert(out_len == len + 8);
  assert(buf[0] & 0x10);

  uint16_t seq = 0;
  assert(read_twcc(buf, out_len, TWCC_ID, &seq));
  assert(seq == 4242);

  sfu_rtp_packet_t p;
  assert(sfu_rtp_packet_parse(buf, out_len, &p));
  assert(p.payload_len == 20);
  assert(memcmp(p.payload, payload_copy, 20) == 0);
}

/* Existing one-byte block with a different element: the TWCC element is
 * appended and the prior element survives. */
static void test_append_to_one_byte_block(void) {
  /* One existing element: id=2, len=1 byte -> header + 1 data + 2 pad = 4. */
  uint8_t ext_body[4] = {(2 << 4) | 0, 0xee, 0, 0};
  uint8_t buf[256];
  size_t len = build_rtp(buf, true, 0xBEDE, ext_body, 4, 16);

  size_t out_len = 0;
  assert(sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 777, &out_len));
  /* new element (3 bytes) pads the block from 4 to 8: grow by 4 */
  assert(out_len == len + 4);

  uint16_t seq = 0;
  assert(read_twcc(buf, out_len, TWCC_ID, &seq));
  assert(seq == 777);

  /* Original element still present: id 2 at block start. */
  sfu_rtp_packet_t p;
  assert(sfu_rtp_packet_parse(buf, out_len, &p));
  assert(p.extension_length == 8);
  assert(p.extension_data[0] == ((2 << 4) | 0));
  assert(p.extension_data[1] == 0xee);
}

/* Existing TWCC element is rewritten in place; length unchanged. */
static void test_rewrite_existing_element(void) {
  /* id=5, len=2 -> 3 bytes + 1 pad = 4. */
  uint8_t ext_body[4] = {(TWCC_ID << 4) | 1, 0x00, 0x2a, 0};
  uint8_t buf[256];
  size_t len = build_rtp(buf, true, 0xBEDE, ext_body, 4, 10);

  size_t out_len = 0;
  assert(sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 999, &out_len));
  assert(out_len == len);

  uint16_t seq = 0;
  assert(read_twcc(buf, out_len, TWCC_ID, &seq));
  assert(seq == 999);
}

/* Publisher-sent TWCC value must be replaced exactly once: writing twice
 * leaves one element with the latest value. */
static void test_rewrite_is_idempotent_single_element(void) {
  uint8_t ext_body[4] = {(TWCC_ID << 4) | 1, 0x12, 0x34, 0};
  uint8_t buf[256];
  size_t len = build_rtp(buf, true, 0xBEDE, ext_body, 4, 10);

  size_t out_len = 0;
  assert(sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 100, &out_len));
  assert(sfu_rtp_ext_write_twcc(buf, out_len, sizeof(buf), TWCC_ID, 101, &out_len));
  assert(out_len == len);

  sfu_rtp_packet_t p;
  assert(sfu_rtp_packet_parse(buf, out_len, &p));
  /* Exactly one element occupies the block. */
  assert(p.extension_length == 4);
  uint16_t seq = 0;
  assert(read_twcc(buf, out_len, TWCC_ID, &seq));
  assert(seq == 101);
}

/* Two-byte-header profile: rewrite and append paths. */
static void test_two_byte_profile(void) {
  /* id=3, len=4 -> 2 + 4 = 6 bytes, padded to 8. */
  uint8_t ext_body[8] = {3, 4, 1, 2, 3, 4, 0, 0};
  uint8_t buf[256];
  size_t len = build_rtp(buf, true, 0x1000, ext_body, 8, 10);

  size_t out_len = 0;
  assert(sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 555, &out_len));
  assert(out_len == len + 4);

  uint16_t seq = 0;
  assert(read_twcc(buf, out_len, TWCC_ID, &seq));
  assert(seq == 555);

  /* Rewrite in place. */
  assert(sfu_rtp_ext_write_twcc(buf, out_len, sizeof(buf), TWCC_ID, 556, &out_len));
  assert(out_len == len + 4);
  assert(read_twcc(buf, out_len, TWCC_ID, &seq));
  assert(seq == 556);
}

/* Capacity limits are enforced: nothing is written when growth would exceed
 * cap. */
static void test_capacity_enforced(void) {
  uint8_t buf[64];
  size_t len = build_rtp(buf, false, 0, NULL, 0, 20); /* 32 bytes */
  uint8_t before[64];
  memcpy(before, buf, len);

  size_t out_len = 0;
  assert(!sfu_rtp_ext_write_twcc(buf, len, len + 7, TWCC_ID, 1, &out_len)); /* need 8 */
  assert(memcmp(buf, before, len) == 0);
  assert(sfu_rtp_ext_write_twcc(buf, len, len + 8, TWCC_ID, 1, &out_len));
  assert(out_len == len + 8);
}

/* Packet with CSRCs: extension block is created after the CSRC list. */
static void test_insert_with_csrcs(void) {
  uint8_t buf[256];
  buf[0] = 0x82; /* V=2, CC=2 */
  buf[1] = 96;
  sfu_write_be16(buf + 2, 1);
  sfu_write_be32(buf + 4, 0x11223344);
  sfu_write_be32(buf + 8, 0x0a0b0c0d);
  sfu_write_be32(buf + 12, 0x11111111);
  sfu_write_be32(buf + 16, 0x22222222);
  memset(buf + 20, 0xab, 8);
  size_t len = 28;

  size_t out_len = 0;
  assert(sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 42, &out_len));
  assert(out_len == len + 8);

  sfu_rtp_packet_t p;
  assert(sfu_rtp_packet_parse(buf, out_len, &p));
  assert(p.csrc_count == 2);
  uint16_t seq = 0;
  assert(read_twcc(buf, out_len, TWCC_ID, &seq));
  assert(seq == 42);
  assert(p.payload_len == 8);
}

/* Rejections: bad id, truncated packet, unknown profile, X bit set with a
 * corrupt block. */
static void test_rejections(void) {
  uint8_t buf[64];
  size_t len = build_rtp(buf, false, 0, NULL, 0, 8);
  size_t out_len = 0;

  assert(!sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), 0, 1, &out_len));  /* id 0 = padding */
  assert(!sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), 15, 1, &out_len)); /* id 15 = reserved */
  assert(!sfu_rtp_ext_write_twcc(buf, 8, sizeof(buf), TWCC_ID, 1, &out_len));

  /* Unknown extension profile. */
  uint8_t ext_body[4] = {0, 0, 0, 0};
  len = build_rtp(buf, true, 0x1234, ext_body, 4, 8);
  assert(!sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 1, &out_len));

  /* X bit set but block truncated. */
  len = build_rtp(buf, false, 0, NULL, 0, 8);
  buf[0] |= 0x10;
  assert(!sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 1, &out_len));
}

/* An existing element with the TWCC id but the wrong length is refused
 * rather than corrupted. */
static void test_wrong_length_element_refused(void) {
  uint8_t ext_body[4] = {(TWCC_ID << 4) | 0, 0xee, 0, 0}; /* id=5, len=1 */
  uint8_t buf[64];
  size_t len = build_rtp(buf, true, 0xBEDE, ext_body, 4, 8);
  size_t out_len = 0;
  assert(!sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), TWCC_ID, 1, &out_len));
}

int main(void) {
  test_insert_into_plain_packet();
  test_append_to_one_byte_block();
  test_rewrite_existing_element();
  test_rewrite_is_idempotent_single_element();
  test_two_byte_profile();
  test_capacity_enforced();
  test_insert_with_csrcs();
  test_rejections();
  test_wrong_length_element_refused();
  printf("test_rtp_ext: OK\n");
  return 0;
}
