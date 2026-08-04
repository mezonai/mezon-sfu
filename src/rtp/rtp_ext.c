#include "rtp/rtp_ext.h"

#include <string.h>

#include "util/netbytes.h"

#define RTP_FIXED_HEADER_LEN 12u
#define RTP_EXT_ONE_BYTE_PROFILE 0xBEDEu
#define RTP_EXT_TWO_BYTE_PROFILE 0x1000u /* 0x100<n>; low nibble is appbits */

/* Returns the offset of the first byte after the fixed header + CSRC list,
 * or 0 when the packet is malformed. */
static size_t rtp_csrc_end(const uint8_t *data, size_t len) {
  if (!data || len < RTP_FIXED_HEADER_LEN || (data[0] >> 6) != 2u) {
    return 0;
  }
  size_t end = RTP_FIXED_HEADER_LEN + (size_t)(data[0] & 0x0fu) * 4u;
  return end <= len ? end : 0;
}

/* Locates the extension block. *ext_off points at the profile field, *ext_len
 * receives the block length in bytes (multiple of 4). Returns false when the
 * packet has no extension or it is truncated. */
static bool rtp_ext_block(const uint8_t *data, size_t len, size_t *ext_off, size_t *ext_len) {
  size_t pos = rtp_csrc_end(data, len);
  if (pos == 0 || !(data[0] & 0x10u)) {
    return false;
  }
  if (len - pos < 4u) {
    return false;
  }
  size_t block_len = (size_t)sfu_read_be16(data + pos + 2u) * 4u;
  if (block_len > len - pos - 4u) {
    return false;
  }
  *ext_off = pos;
  *ext_len = block_len;
  return true;
}

/* Finds an existing one-byte-header (RFC 8285 §4.2) element with id `ext_id`.
 * On success *elem_off points at the element's ID/len byte and *elem_size is
 * the full element size (header + data). Walks defensively: a malformed
 * element header stops the scan without matching. */
static bool one_byte_find(const uint8_t *ext, size_t ext_len, uint8_t ext_id, size_t *elem_off, size_t *elem_size) {
  size_t pos = 0;
  while (pos < ext_len) {
    uint8_t b = ext[pos];
    if (b == 0) { /* padding */
      pos++;
      continue;
    }
    uint8_t id = b >> 4;
    size_t data_len = (size_t)(b & 0x0fu) + 1u;
    if (id == 15u) { /* reserved; stop per RFC 8285 */
      return false;
    }
    if (pos + 1u + data_len > ext_len) {
      return false;
    }
    if (id == ext_id) {
      *elem_off = pos;
      *elem_size = 1u + data_len;
      return true;
    }
    pos += 1u + data_len;
  }
  return false;
}

/* Finds an existing two-byte-header (RFC 8285 §4.3) element with id
 * `ext_id`. */
static bool two_byte_find(const uint8_t *ext, size_t ext_len, uint8_t ext_id, size_t *elem_off, size_t *elem_size) {
  size_t pos = 0;
  while (pos < ext_len) {
    uint8_t id = ext[pos];
    if (id == 0) { /* padding */
      pos++;
      continue;
    }
    if (pos + 2u > ext_len) {
      return false;
    }
    size_t data_len = ext[pos + 1u];
    if (pos + 2u + data_len > ext_len) {
      return false;
    }
    if (id == ext_id) {
      *elem_off = pos;
      *elem_size = 2u + data_len;
      return true;
    }
    pos += 2u + data_len;
  }
  return false;
}

/* Appends `elem_len` bytes of extension element at the tail of the block,
 * zero-padding the block to its new 4-byte-aligned length. Grows the packet
 * by moving the payload; the caller has already verified capacity and that
 * the block can be extended (length-field limit). */
static void ext_block_grow(uint8_t *data, size_t len, size_t ext_off, size_t ext_len, size_t new_ext_len) {
  size_t payload_off = ext_off + 4u + ext_len;
  size_t tail = len - payload_off;
  size_t grow = new_ext_len - ext_len;
  memmove(data + payload_off + grow, data + payload_off, tail);
  memset(data + payload_off, 0, grow);
  sfu_write_be16(data + ext_off + 2u, (uint16_t)(new_ext_len / 4u));
}

bool sfu_rtp_ext_write_twcc(uint8_t *data, size_t len, size_t cap, uint8_t ext_id, uint16_t twcc_seq, size_t *io_len) {
  if (!data || !io_len || ext_id == 0u || ext_id > 14u || len < RTP_FIXED_HEADER_LEN || len > cap) {
    return false;
  }

  size_t ext_off = 0;
  size_t ext_len = 0;
  uint16_t profile = 0;
  bool has_ext = rtp_ext_block(data, len, &ext_off, &ext_len);
  if (has_ext) {
    profile = sfu_read_be16(data + ext_off);
  }

  if (has_ext && profile == RTP_EXT_ONE_BYTE_PROFILE) {
    size_t elem_off = 0;
    size_t elem_size = 0;
    if (one_byte_find(data + ext_off + 4u, ext_len, ext_id, &elem_off, &elem_size)) {
      if (elem_size != 1u + SFU_TWCC_EXT_LEN) {
        return false; /* same ID with unexpected length; refuse to corrupt it */
      }
      sfu_write_be16(data + ext_off + 4u + elem_off + 1u, twcc_seq);
      *io_len = len;
      return true;
    }
    /* Append: 1 header byte + 2 data bytes, block padded to 4. */
    size_t needed = 1u + SFU_TWCC_EXT_LEN;
    size_t new_ext_len = (ext_len + needed + 3u) & ~(size_t)3u;
    size_t grow = new_ext_len - ext_len;
    if (new_ext_len / 4u > 0xFFFFu || len + grow > cap) {
      return false;
    }
    size_t insert_at = ext_off + 4u + ext_len;
    ext_block_grow(data, len, ext_off, ext_len, new_ext_len);
    data[insert_at] = (uint8_t)((ext_id << 4) | (SFU_TWCC_EXT_LEN - 1u));
    sfu_write_be16(data + insert_at + 1u, twcc_seq);
    *io_len = len + grow;
    return true;
  }

  if (has_ext && (profile & 0xFFF0u) == RTP_EXT_TWO_BYTE_PROFILE) {
    size_t elem_off = 0;
    size_t elem_size = 0;
    if (two_byte_find(data + ext_off + 4u, ext_len, ext_id, &elem_off, &elem_size)) {
      if (elem_size != 2u + SFU_TWCC_EXT_LEN) {
        return false;
      }
      sfu_write_be16(data + ext_off + 4u + elem_off + 2u, twcc_seq);
      *io_len = len;
      return true;
    }
    /* Append: 2 header bytes + 2 data bytes = 4, always 4-aligned. */
    size_t new_ext_len = ext_len + 4u;
    if (new_ext_len / 4u > 0xFFFFu || len + 4u > cap) {
      return false;
    }
    size_t insert_at = ext_off + 4u + ext_len;
    ext_block_grow(data, len, ext_off, ext_len, new_ext_len);
    data[insert_at] = ext_id;
    data[insert_at + 1u] = SFU_TWCC_EXT_LEN;
    sfu_write_be16(data + insert_at + 2u, twcc_seq);
    *io_len = len + 4u;
    return true;
  }

  if (has_ext) {
    return false; /* unknown extension profile; never guess */
  }

  /* No extension block: create a one-byte-header block holding exactly the
   * transport-cc element. Total growth: 4 (profile+length) + 4 (element +
   * pad). The fixed header's X bit must not already be claimed by a block we
   * failed to parse — rtp_ext_block only returns false for X=0 or a
   * truncated block; the latter is rejected by refusing when X is set. */
  if (data[0] & 0x10u) {
    return false;
  }
  size_t csrc_end = rtp_csrc_end(data, len);
  if (csrc_end == 0 || len + 8u > cap) {
    return false;
  }
  memmove(data + csrc_end + 8u, data + csrc_end, len - csrc_end);
  data[0] |= 0x10u;
  sfu_write_be16(data + csrc_end, RTP_EXT_ONE_BYTE_PROFILE);
  sfu_write_be16(data + csrc_end + 2u, 1u); /* one 32-bit word */
  data[csrc_end + 4u] = (uint8_t)((ext_id << 4) | (SFU_TWCC_EXT_LEN - 1u));
  sfu_write_be16(data + csrc_end + 5u, twcc_seq);
  data[csrc_end + 7u] = 0; /* padding */
  *io_len = len + 8u;
  return true;
}
