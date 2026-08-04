#ifndef SFU_VP9_PARSER_H
#define SFU_VP9_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum P_DIFF reference indices (RFC 9628: at most 3 in flexible mode). */
#define SFU_VP9_MAX_P_DIFF 3

typedef struct {
  bool i_bit; /* Picture ID present */
  bool p_bit; /* Inter-picture predicted frame */
  bool l_bit; /* Layer indices present (Essential for SVC) */
  bool f_bit; /* Flexible mode */
  bool b_bit; /* Start of a frame */
  bool e_bit; /* End of a frame */
  bool v_bit; /* Scalability Structure (SS) data present */
  bool z_bit; /* Not a reference frame for upper spatial layers */

  /* Picture ID (if I=1) */
  uint16_t picture_id;

  /* Layer Indices (if L=1) */
  uint8_t tid;   /* Temporal layer ID */
  uint8_t u_bit; /* Switching point - crucial for layer upgrades */
  uint8_t sid;   /* Spatial layer ID */
  uint8_t d_bit; /* Inter-layer dependency used */

  /* TL0PICIDX (if L=1 && F=0) */
  uint8_t tl0picidx;

  /* Reference indices (if P=1 && F=1): up to SFU_VP9_MAX_P_DIFF 7-bit diffs */
  uint8_t p_diff[SFU_VP9_MAX_P_DIFF];
  uint8_t p_diff_count;

  /* Length of the parsed descriptor so we know where actual VP9 frame data starts */
  size_t header_length;
} sfu_vp9_descriptor_t;

int sfu_parse_vp9_descriptor(const uint8_t *payload, size_t len, sfu_vp9_descriptor_t *out);

#endif  // SFU_VP9_PARSER_H
