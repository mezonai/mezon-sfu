#include "rtcp/rtcp_kf.h"
#include <string.h>

// Builds a Picture Loss Indication (PLI) packet
// Size: 12 bytes
int sfu_rtcp_build_pli(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *buffer, size_t cap) {
  if (cap < 12) {
    return -1;
  }

  buffer[0] = 0x81;  // V=2, P=0, FMT=1 (PLI)
  buffer[1] = 206;   // PT=206 (PSFB)

  uint16_t len = htons(2);  // Length = (12 bytes / 4) - 1 = 2
  memcpy(buffer + 2, &len, 2);

  uint32_t ssrc = htonl(sender_ssrc);
  memcpy(buffer + 4, &ssrc, 4);

  ssrc = htonl(media_ssrc);
  memcpy(buffer + 8, &ssrc, 4);

  return 12;
}

// Builds a Full Intra Request (FIR) packet
// Size: 20 bytes
int sfu_rtcp_build_fir(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *fir_seq, uint8_t *buffer, size_t cap) {
  if (cap < 20) {
    return -1;
  }

  buffer[0] = 0x84;  // V=2, P=0, FMT=4 (FIR)
  buffer[1] = 206;   // PT=206 (PSFB)

  uint16_t len = htons(4);  // Length = (20 bytes / 4) - 1 = 4
  memcpy(buffer + 2, &len, 2);

  uint32_t ssrc = htonl(sender_ssrc);
  memcpy(buffer + 4, &ssrc, 4);

  ssrc = htonl(media_ssrc);  // RFC 5104 says unused, usually 0 or media_ssrc
  memcpy(buffer + 8, &ssrc, 4);

  // FCI Block
  ssrc = htonl(media_ssrc);  // The SSRC we need a keyframe from
  memcpy(buffer + 12, &ssrc, 4);

  buffer[16] = (*fir_seq)++;  // Command Sequence Number (must increment per request)
  buffer[17] = 0;             // Reserved
  buffer[18] = 0;             // Reserved
  buffer[19] = 0;             // Reserved

  return 20;
}

int sfu_rtcp_build_remb(uint32_t sender_ssrc, uint64_t bitrate_bps, const uint32_t *media_ssrcs, uint8_t media_ssrc_count, uint8_t *buffer,
                        size_t cap) {
  if (!buffer || !media_ssrcs || media_ssrc_count == 0) {
    return -1;
  }

  size_t packet_len = 20u + (size_t)media_ssrc_count * 4u;
  if (packet_len > cap) {
    return -1;
  }

  uint8_t exponent = 0;
  uint64_t mantissa = bitrate_bps;
  while (mantissa > 0x3FFFFu && exponent < 63) {
    mantissa >>= 1;
    exponent++;
  }
  if (mantissa > 0x3FFFFu) {
    mantissa = 0x3FFFFu;
  }

  buffer[0] = 0x8F;  // V=2, P=0, FMT=15 (application layer feedback)
  buffer[1] = 206;   // PT=206 (PSFB)
  uint16_t length = htons((uint16_t)(packet_len / 4u - 1u));
  memcpy(buffer + 2, &length, 2);

  uint32_t ssrc = htonl(sender_ssrc);
  memcpy(buffer + 4, &ssrc, 4);
  memset(buffer + 8, 0, 4);  // Media source SSRC is unused for REMB.
  memcpy(buffer + 12, "REMB", 4);
  buffer[16] = media_ssrc_count;
  uint32_t encoded = ((uint32_t)exponent << 18) | (uint32_t)mantissa;
  buffer[17] = (uint8_t)(encoded >> 16);
  buffer[18] = (uint8_t)(encoded >> 8);
  buffer[19] = (uint8_t)encoded;

  for (uint8_t i = 0; i < media_ssrc_count; i++) {
    ssrc = htonl(media_ssrcs[i]);
    memcpy(buffer + 20u + (size_t)i * 4u, &ssrc, 4);
  }
  return (int)packet_len;
}
