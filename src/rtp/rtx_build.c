#include "rtp/rtx_build.h"

#include <string.h>

#include "rtp/rtp_packet.h"
#include "util/netbytes.h"

bool sfu_rtx_build(const uint8_t *orig, size_t orig_len, uint8_t rtx_pt,
                   uint16_t rtx_seq, uint32_t rtx_ssrc, uint8_t *out,
                   size_t out_cap, size_t *out_len) {
  sfu_rtp_packet_t packet;
  size_t built_len;

  if (!orig || !out || !out_len || rtx_pt > 127u ||
      !sfu_rtp_packet_parse(orig, orig_len, &packet) ||
      orig_len < packet.header_len || orig_len > SIZE_MAX - 2u ||
      packet.payload_len > SIZE_MAX - packet.header_len - 2u) {
    return false;
  }

  built_len = packet.header_len + 2u + packet.payload_len;
  /* Conservatively require room for the original packet plus the OSN before
   * writing, even when stripping original padding makes built_len smaller. */
  if (orig_len + 2u > out_cap) {
    return false;
  }

  memcpy(out, orig, packet.header_len);
  out[0] &= (uint8_t)~0x20u;
  (void)sfu_rtp_packet_set_pt(out, packet.header_len, rtx_pt);
  (void)sfu_rtp_packet_set_seq(out, packet.header_len, rtx_seq);
  (void)sfu_rtp_packet_set_ssrc(out, packet.header_len, rtx_ssrc);
  sfu_write_be16(out + packet.header_len, packet.sequence_number);
  memcpy(out + packet.header_len + 2u, packet.payload, packet.payload_len);
  *out_len = built_len;
  return true;
}
