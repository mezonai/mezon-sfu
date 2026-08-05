#ifndef SFU_RTP_PACKET_H
#define SFU_RTP_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sfu_rtp_packet {
  uint8_t version;
  bool padding;
  bool extension;
  uint8_t csrc_count;
  bool marker;
  uint8_t payload_type;
  uint16_t sequence_number;
  uint32_t timestamp;
  uint32_t ssrc;
  uint16_t extension_profile;
  const uint8_t *extension_data;
  size_t extension_length;
  size_t header_len;
  const uint8_t *payload;
  size_t payload_len;
} sfu_rtp_packet_t;

bool sfu_rtp_packet_parse(const uint8_t *data, size_t len, sfu_rtp_packet_t *packet);
bool sfu_rtp_packet_set_pt(uint8_t *data, size_t len, uint8_t payload_type);
bool sfu_rtp_packet_set_seq(uint8_t *data, size_t len, uint16_t sequence_number);
bool sfu_rtp_packet_set_ssrc(uint8_t *data, size_t len, uint32_t ssrc);

#endif /* SFU_RTP_PACKET_H */
