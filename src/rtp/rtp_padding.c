#include "rtp/rtp_padding.h"

#include <string.h>

#include "rtp/rtp_ext.h"
#include "util/netbytes.h"

#define RTP_FIXED_HEADER_LEN 12u

bool sfu_rtp_padding_build(uint8_t *out, size_t out_cap, uint8_t payload_type,
                           uint16_t seq, uint32_t timestamp, uint32_t ssrc,
                           uint16_t padding_bytes, uint8_t twcc_ext_id,
                           uint16_t twcc_seq, size_t *out_len) {
  if (!out || !out_len || payload_type > 127u || ssrc == 0) {
    return false;
  }
  if (twcc_ext_id > 14u) {
    return false;
  }
  if (padding_bytes == 0) {
    padding_bytes = SFU_RTP_PADDING_DEFAULT_BYTES;
  }
  if (padding_bytes > SFU_RTP_PADDING_MAX_BYTES) {
    return false;
  }

  size_t header_len = RTP_FIXED_HEADER_LEN;
  if (header_len > out_cap) {
    return false;
  }

  out[0] = 0x80u;
  out[1] = payload_type & 0x7fu;
  sfu_write_be16(out + 2, seq);
  sfu_write_be32(out + 4, timestamp);
  sfu_write_be32(out + 8, ssrc);

  if (twcc_ext_id > 0) {
    size_t ext_written_len = header_len;
    if (!sfu_rtp_ext_write_twcc(out, header_len, out_cap, twcc_ext_id, twcc_seq, &ext_written_len)) {
      return false;
    }
    header_len = ext_written_len;
  }

  if (header_len + padding_bytes > out_cap) {
    return false;
  }

  /* Set RFC 3550 padding bit (0x20) */
  out[0] |= 0x20u;

  memset(out + header_len, 0, padding_bytes);
  out[header_len + padding_bytes - 1u] = (uint8_t)padding_bytes;

  *out_len = header_len + padding_bytes;
  return true;
}
