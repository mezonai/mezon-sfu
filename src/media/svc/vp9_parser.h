#ifndef SFU_VP9_PARSER_H
#define SFU_VP9_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_VP9_MAX_P_DIFF 3

typedef struct {
  bool i_bit;
  bool p_bit;
  bool l_bit;
  bool f_bit;
  bool b_bit;
  bool e_bit;
  bool v_bit;
  bool z_bit;
  uint16_t picture_id;
  uint8_t tid;
  uint8_t u_bit;
  uint8_t sid;
  uint8_t d_bit;
  uint8_t tl0picidx;
  uint8_t p_diff[SFU_VP9_MAX_P_DIFF];
  uint8_t p_diff_count;
  size_t header_length;
} sfu_vp9_descriptor_t;

int sfu_parse_vp9_descriptor(const uint8_t *payload, size_t len, sfu_vp9_descriptor_t *out);

#endif  // SFU_VP9_PARSER_H
