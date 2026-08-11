#include "rtp/rtp_packet.h"

#include <string.h>

#include "util/netbytes.h"

#define RTP_FIXED_HEADER_LEN 12u

bool sfu_rtp_packet_parse(const uint8_t *data, size_t len, sfu_rtp_packet_t *packet) {
  size_t header_len;
  size_t payload_span;

  if (!data || !packet || len < RTP_FIXED_HEADER_LEN) {
    return false;
  }

  memset(packet, 0, sizeof(*packet));
  packet->version = data[0] >> 6;
  if (packet->version != 2) {
    return false;
  }

  packet->padding = (data[0] & 0x20u) != 0;
  packet->extension = (data[0] & 0x10u) != 0;
  packet->csrc_count = data[0] & 0x0fu;
  packet->marker = (data[1] & 0x80u) != 0;
  packet->payload_type = data[1] & 0x7fu;
  packet->sequence_number = sfu_read_be16(data + 2);
  packet->timestamp = sfu_read_be32(data + 4);
  packet->ssrc = sfu_read_be32(data + 8);

  header_len = RTP_FIXED_HEADER_LEN + (size_t)packet->csrc_count * 4u;
  if (header_len > len) {
    return false;
  }

  if (packet->extension) {
    size_t extension_bytes;
    if (len - header_len < 4u) {
      return false;
    }
    packet->extension_profile = sfu_read_be16(data + header_len);
    extension_bytes = (size_t)sfu_read_be16(data + header_len + 2u) * 4u;
    header_len += 4u;
    if (extension_bytes > len - header_len) {
      return false;
    }
    packet->extension_data = data + header_len;
    packet->extension_length = extension_bytes;
    header_len += extension_bytes;
  }

  packet->header_len = header_len;
  packet->payload = data + header_len;
  payload_span = len - header_len;
  packet->payload_len = payload_span;

  if (packet->padding) {
    uint8_t padding_count;
    if (payload_span == 0) {
      return false;
    }
    padding_count = data[len - 1u];
    if (padding_count == 0 || (size_t)padding_count > payload_span) {
      return false;
    }
    packet->payload_len -= padding_count;
  }

  return true;
}

bool sfu_rtp_packet_set_pt(uint8_t *data, size_t len, uint8_t payload_type) {
  if (!data || len < RTP_FIXED_HEADER_LEN || payload_type > 127u) {
    return false;
  }
  data[1] = (uint8_t)((data[1] & 0x80u) | payload_type);
  return true;
}

bool sfu_rtp_packet_set_marker(uint8_t *data, size_t len, bool marker) {
  if (!data || len < RTP_FIXED_HEADER_LEN) {
    return false;
  }
  if (marker) {
    data[1] |= 0x80u;
  } else {
    data[1] &= 0x7fu;
  }
  return true;
}

bool sfu_rtp_packet_set_seq(uint8_t *data, size_t len, uint16_t sequence_number) {
  if (!data || len < RTP_FIXED_HEADER_LEN) {
    return false;
  }
  sfu_write_be16(data + 2, sequence_number);
  return true;
}

bool sfu_rtp_packet_set_ssrc(uint8_t *data, size_t len, uint32_t ssrc) {
  if (!data || len < RTP_FIXED_HEADER_LEN) {
    return false;
  }
  sfu_write_be32(data + 8, ssrc);
  return true;
}
