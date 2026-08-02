#include "media/svc/vp9_parser.h"

int sfu_parse_vp9_descriptor(const uint8_t *payload, size_t len, sfu_vp9_descriptor_t *out) {
  if (!payload || !out || len == 0) {
    return -1;
  }

  size_t offset = 0;
  uint8_t first_byte = payload[offset++];

  out->i_bit = (first_byte >> 7) & 0x01;
  out->p_bit = (first_byte >> 6) & 0x01;
  out->l_bit = (first_byte >> 5) & 0x01;
  out->f_bit = (first_byte >> 4) & 0x01;
  out->b_bit = (first_byte >> 3) & 0x01;
  out->e_bit = (first_byte >> 2) & 0x01;
  out->v_bit = (first_byte >> 1) & 0x01;
  out->z_bit = (first_byte & 0x01);

  out->picture_id = 0;
  out->tid = 0;
  out->sid = 0;
  out->u_bit = 0;
  out->d_bit = 0;

  if (out->i_bit) {
    if (offset >= len) {
      return -1;
    }
    uint8_t pid_first_byte = payload[offset++];

    if ((pid_first_byte >> 7) & 0x01) {
      if (offset >= len) {
        return -1;
      }
      uint8_t pid_second_byte = payload[offset++];
      out->picture_id = ((pid_first_byte & 0x7F) << 8) | pid_second_byte;
    } else {
      out->picture_id = pid_first_byte & 0x7F;
    }
  }

  if (out->l_bit) {
    if (offset >= len) {
      return -1;
    }
    uint8_t layer_byte = payload[offset++];

    out->tid = (layer_byte >> 5) & 0x07;
    out->u_bit = (layer_byte >> 4) & 0x01;
    out->sid = (layer_byte >> 1) & 0x07;
    out->d_bit = (layer_byte & 0x01);
  }

  out->header_length = offset;

  return 0;
}
