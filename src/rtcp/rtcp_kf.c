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
