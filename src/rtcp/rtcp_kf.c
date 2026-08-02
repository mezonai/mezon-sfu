#include "rtcp/rtcp_kf.h"

// Builds a Picture Loss Indication (PLI) packet
// Size: 12 bytes
int sfu_rtcp_build_pli(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *buffer, size_t cap) {
  if (cap < 12) {
    return -1;
  }

  buffer[0] = 0x81;                      // V=2, P=0, FMT=1 (PLI)
  buffer[1] = 206;                       // PT=206 (PSFB)
  *(uint16_t *)(buffer + 2) = htons(2);  // Length = (12 bytes / 4) - 1 = 2

  *(uint32_t *)(buffer + 4) = htonl(sender_ssrc);
  *(uint32_t *)(buffer + 8) = htonl(media_ssrc);

  return 12;
}

// Builds a Full Intra Request (FIR) packet
// Size: 20 bytes
int sfu_rtcp_build_fir(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *fir_seq, uint8_t *buffer, size_t cap) {
  if (cap < 20) {
    return -1;
  }

  buffer[0] = 0x84;                      // V=2, P=0, FMT=4 (FIR)
  buffer[1] = 206;                       // PT=206 (PSFB)
  *(uint16_t *)(buffer + 2) = htons(4);  // Length = (20 bytes / 4) - 1 = 4

  *(uint32_t *)(buffer + 4) = htonl(sender_ssrc);
  *(uint32_t *)(buffer + 8) = htonl(media_ssrc);  // RFC 5104 says unused, usually 0 or media_ssrc

  // FCI Block
  *(uint32_t *)(buffer + 12) = htonl(media_ssrc);  // The SSRC we need a keyframe from
  buffer[16] = (*fir_seq)++;                       // Command Sequence Number (must increment per request)
  buffer[17] = 0;                                  // Reserved
  buffer[18] = 0;                                  // Reserved
  buffer[19] = 0;                                  // Reserved

  return 20;
}
