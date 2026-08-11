#include "media/svc/svc_parser.h"
#include <string.h>

int sfu_parse_vp8_descriptor(const uint8_t *payload, size_t payload_len, sfu_vp8_descriptor_t *desc) {
  if (!payload || !desc || payload_len < 1) {
    return -1;
  }

  memset(desc, 0, sizeof(sfu_vp8_descriptor_t));

  size_t offset = 0;

  uint8_t b = payload[offset++];
  desc->x = (b & 0x80) != 0;
  desc->n = (b & 0x20) != 0;
  desc->s = (b & 0x10) != 0;
  desc->part_id = b & 0x0F;

  if (desc->x) {
    if (offset >= payload_len) {
      return -1;
    }
    uint8_t x_byte = payload[offset++];

    desc->i = (x_byte & 0x80) != 0;
    desc->l = (x_byte & 0x40) != 0;
    desc->t = (x_byte & 0x20) != 0;
    desc->k = (x_byte & 0x10) != 0;

    if (desc->i) {
      if (offset >= payload_len) {
        return -1;
      }
      uint8_t pid_byte = payload[offset++];

      if (pid_byte & 0x80) {
        if (offset >= payload_len) {
          return -1;
        }
        desc->picture_id = ((pid_byte & 0x7F) << 8) | payload[offset++];
      } else {
        desc->picture_id = pid_byte & 0x7F;
      }
    }

    if (desc->l) {
      if (offset >= payload_len) {
        return -1;
      }
      desc->tl0picidx = payload[offset++];
    }

    if (desc->t || desc->k) {
      if (offset >= payload_len) {
        return -1;
      }
      uint8_t t_k_byte = payload[offset++];
      desc->tid = (t_k_byte & 0xC0) >> 6;
      desc->y = (t_k_byte & 0x20) != 0;
      desc->keyidx = t_k_byte & 0x1F;
    }
  }

  desc->header_len = offset;

  desc->is_keyframe = false;

  if (desc->s && desc->part_id == 0 && offset < payload_len) {
    uint8_t vp8_payload_first_byte = payload[offset];

    if ((vp8_payload_first_byte & 0x01) == 0) {
      desc->is_keyframe = true;
    }
  }

  return 0;
}

int sfu_parse_vp9_descriptor(const uint8_t *payload, size_t len, sfu_vp9_descriptor_t *out) {
  if (!payload || !out || len == 0) {
    return -1;
  }

  memset(out, 0, sizeof(*out));

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
  out->tl0picidx = 0;
  out->p_diff_count = 0;
  out->p_diff[0] = 0;
  out->p_diff[1] = 0;
  out->p_diff[2] = 0;

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

    if (!out->f_bit) {
      if (offset >= len) {
        return -1;
      }
      out->tl0picidx = payload[offset++];
    }
  }

  if (out->p_bit && out->f_bit) {
    do {
      if (offset >= len) {
        return -1;
      }
      uint8_t diff = payload[offset++];
      if (out->p_diff_count < SFU_VP9_MAX_P_DIFF) {
        out->p_diff[out->p_diff_count] = diff & 0x7F;
      }
      out->p_diff_count++;
      if ((diff & 0x80) == 0) {
        break;
      }
    } while (out->p_diff_count < SFU_VP9_MAX_P_DIFF);

    if (out->p_diff_count == SFU_VP9_MAX_P_DIFF && (payload[offset - 1] & 0x80)) {
      return -1;
    }
  }

  if (out->v_bit) {
    if (offset >= len) {
      return -1;
    }
    uint8_t ss_first = payload[offset++];
    uint8_t n_s = (ss_first >> 5) & 0x07;
    bool y_bit = (ss_first >> 4) & 0x01;
    bool g_bit = (ss_first >> 3) & 0x01;

    if (y_bit) {
      size_t res_bytes = (size_t)(n_s + 1) * 4;
      if (len - offset < res_bytes) {
        return -1;
      }
      offset += res_bytes;
    }

    if (g_bit) {
      if (offset >= len) {
        return -1;
      }
      uint8_t n_g = payload[offset++];
      for (uint8_t i = 0; i < n_g; i++) {
        if (offset >= len) {
          return -1;
        }
        uint8_t pg_byte = payload[offset++];
        uint8_t r = (pg_byte >> 2) & 0x03;
        if (len - offset < r) {
          return -1;
        }
        offset += r;
      }
    }
  }

  out->header_length = offset;

  return 0;
}
