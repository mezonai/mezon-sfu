#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "media/svc/vp9_parser.h"

#define VP9_I 0x80
#define VP9_P 0x40
#define VP9_L 0x20
#define VP9_F 0x10
#define VP9_B 0x08
#define VP9_E 0x04
#define VP9_V 0x02
#define VP9_Z 0x01

static void test_minimal_no_extensions(void) {
  uint8_t buf[] = {VP9_B | VP9_E, 0xAA};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(!d.i_bit && !d.p_bit && !d.l_bit && !d.f_bit && !d.v_bit);
  assert(d.b_bit && d.e_bit && !d.z_bit);
  assert(d.header_length == 1);
  assert(d.picture_id == 0 && d.sid == 0 && d.tid == 0);
  assert(d.p_diff_count == 0);
}

static void test_picture_id_7bit(void) {
  uint8_t buf[] = {VP9_I | VP9_B | VP9_E, 0x55, 0xAA};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.i_bit);
  assert(d.picture_id == 0x55);
  assert(d.header_length == 2);
}

static void test_picture_id_15bit(void) {
  /* M=1: (0x23 << 8) | 0x77 = 0x2377. */
  uint8_t buf[] = {VP9_I | VP9_B | VP9_E, 0x80 | 0x23, 0x77, 0xAA};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.picture_id == 0x2377);
  assert(d.header_length == 3);
}

static void test_layer_indices_nonflexible_tl0picidx(void) {
  /* L=1, F=0: layer byte then TL0PICIDX. TID=2 U=1, SID=1 D=1. */
  uint8_t layer = (2u << 5) | (1u << 4) | (1u << 1) | 1u;
  uint8_t buf[] = {VP9_P | VP9_L | VP9_B, layer, 0x42, 0xAA};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.l_bit && !d.f_bit);
  assert(d.tid == 2 && d.u_bit == 1);
  assert(d.sid == 1 && d.d_bit == 1);
  assert(d.tl0picidx == 0x42);
  assert(d.header_length == 3);
  /* P=1 but F=0: no P_DIFF chain. */
  assert(d.p_diff_count == 0);
}

static void test_layer_indices_flexible_no_tl0picidx(void) {
  /* L=1, F=1: no TL0PICIDX byte; P_DIFF chain follows the layer byte. */
  uint8_t layer = (1u << 5) | (0u << 4) | (2u << 1) | 0u;
  uint8_t buf[] = {VP9_I | VP9_P | VP9_L | VP9_F, 0x10, layer, 0x03, 0xAA};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.f_bit);
  assert(d.tid == 1 && d.sid == 2);
  assert(d.picture_id == 0x10);
  assert(d.p_diff_count == 1);
  assert(d.p_diff[0] == 0x03);
  assert(d.header_length == 4);
}

static void test_p_diff_chains(void) {
  uint8_t layer = 0; /* TID=0 U=0 SID=0 D=0 */
  sfu_vp9_descriptor_t d;

  /* Chain of 1: high bit clear. */
  {
    uint8_t buf[] = {VP9_P | VP9_L | VP9_F, layer, 0x05, 0xAA};
    assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
    assert(d.p_diff_count == 1);
    assert(d.p_diff[0] == 0x05);
    assert(d.header_length == 3);
  }

  /* Chain of 2: first has continuation bit. */
  {
    uint8_t buf[] = {VP9_P | VP9_L | VP9_F, layer, 0x80 | 0x01, 0x02, 0xAA};
    assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
    assert(d.p_diff_count == 2);
    assert(d.p_diff[0] == 0x01 && d.p_diff[1] == 0x02);
    assert(d.header_length == 4);
  }

  /* Chain of 3 (maximum). */
  {
    uint8_t buf[] = {VP9_P | VP9_L | VP9_F, layer, 0x80 | 0x01, 0x80 | 0x02, 0x03, 0xAA};
    assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
    assert(d.p_diff_count == 3);
    assert(d.p_diff[0] == 0x01 && d.p_diff[1] == 0x02 && d.p_diff[2] == 0x03);
    assert(d.header_length == 5);
  }

  /* Malformed: continuation bit set on the third P_DIFF. */
  {
    uint8_t buf[] = {VP9_P | VP9_L | VP9_F, layer, 0x80 | 0x01, 0x80 | 0x02, 0x80 | 0x03, 0xAA};
    assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == -1);
  }
}

static void test_scalability_structure_minimal(void) {
  /* V=1, Y=0, G=0: only the N_S/Y/G byte. N_S=2. */
  uint8_t ss = (2u << 5);
  uint8_t buf[] = {VP9_B | VP9_V, ss, 0xAA};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.v_bit);
  assert(d.header_length == 2);
}

static void test_scalability_structure_resolutions(void) {
  sfu_vp9_descriptor_t d;

  /* Y=1, N_S=0: one WIDTH/HEIGHT pair (2 bytes). */
  {
    uint8_t ss = (0u << 5) | (1u << 4);
    uint8_t buf[] = {VP9_B | VP9_V, ss, 0x01, 0x40, 0xAA};
    assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
    assert(d.header_length == 4);
  }

  /* Y=1, N_S=2: three WIDTH/HEIGHT pairs (6 bytes). */
  {
    uint8_t ss = (2u << 5) | (1u << 4);
    uint8_t buf[] = {VP9_B | VP9_V, ss, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xAA};
    assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
    assert(d.header_length == 8);
  }
}

static void test_scalability_structure_group_of_pictures(void) {
  /* V=1, Y=0, G=1, N_S=0, N_G=2.
   * PG entry 1: T=1 U=1 R=0 -> no reference diffs.
   * PG entry 2: T=0 U=0 R=2 -> 2 reference diff bytes. */
  uint8_t ss = (0u << 5) | (1u << 3);
  uint8_t pg1 = (1u << 3) | (1u << 2) | (0u << 1);
  uint8_t pg2 = (0u << 3) | (0u << 2) | (2u << 1);
  uint8_t buf[] = {VP9_B | VP9_V, ss, 2, pg1, pg2, 0x11, 0x22, 0xAA};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.header_length == 8);
}

static void test_scalability_structure_full(void) {
  /* V=1, Y=1, G=1, N_S=1, N_G=1 with R=1. */
  uint8_t ss = (1u << 5) | (1u << 4) | (1u << 3);
  uint8_t pg = (0u << 3) | (1u << 2) | (1u << 1); /* T=0 U=1 R=1 */
  uint8_t buf[] = {VP9_B | VP9_V, ss, 0x0A, 0x0B, 0x0C, 0x0D, 1, pg, 0x33, 0xAA, 0xBB};
  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.header_length == 11);
}

static void test_every_flag_combination(void) {
  /* All 32 combinations of I/P/L/F/V get a well-formed descriptor with one
   * payload byte; header_length must land exactly before the payload. */
  for (uint32_t flags = 0; flags < 32; flags++) {
    uint8_t buf[32];
    size_t n = 0;
    uint8_t first = VP9_B | VP9_E;
    bool i_bit = flags & 1, p_bit = flags & 2, l_bit = flags & 4, f_bit = flags & 8, v_bit = flags & 16;
    if (i_bit) {
      first |= VP9_I;
    }
    if (p_bit) {
      first |= VP9_P;
    }
    if (l_bit) {
      first |= VP9_L;
    }
    if (f_bit) {
      first |= VP9_F;
    }
    if (v_bit) {
      first |= VP9_V;
    }
    buf[n++] = first;
    size_t expected = n;

    if (i_bit) {
      buf[n++] = 0x24; /* 7-bit picture ID */
      expected += 1;
    }
    if (l_bit) {
      buf[n++] = (uint8_t)((1u << 5) | (1u << 1)); /* TID=1 SID=1 */
      expected += 1;
      if (!f_bit) {
        buf[n++] = 0x77; /* TL0PICIDX */
        expected += 1;
      }
    }
    if (p_bit && f_bit) {
      buf[n++] = 0x80 | 0x01;
      buf[n++] = 0x02;
      expected += 2;
    }
    if (v_bit) {
      buf[n++] = (1u << 4); /* N_S=0 Y=1 G=0 */
      buf[n++] = 0x01;
      buf[n++] = 0x02; /* one resolution pair */
      expected += 3;
    }
    buf[n++] = 0xAA; /* VP9 bitstream byte */

    sfu_vp9_descriptor_t d;
    assert(sfu_parse_vp9_descriptor(buf, n, &d) == 0);
    assert(d.header_length == expected);

    /* Every truncation of this descriptor must fail. */
    for (size_t cut = 0; cut < expected; cut++) {
      assert(sfu_parse_vp9_descriptor(buf, cut, &d) == -1);
    }
  }
}

static void test_maximal_descriptor_truncation_sweep(void) {
  /* Maximal descriptor: I=1 (15-bit PID), L=1, F=1, P=1 with 3 P_DIFFs,
   * V=1 with Y=1 (N_S=2), G=1 (N_G=2, R=0 and R=2). */
  uint8_t buf[] = {
      VP9_I | VP9_P | VP9_L | VP9_F | VP9_B | VP9_E | VP9_V,
      0x80 | 0x11,
      0x22,                                              /* 15-bit picture ID 0x1122 */
      (uint8_t)((2u << 5) | (1u << 4) | (1u << 1) | 1u), /* TID=2 U=1 SID=1 D=1 */
      0x80 | 0x01,
      0x80 | 0x02,
      0x03,                              /* P_DIFF chain of 3 */
      (2u << 5) | (1u << 4) | (1u << 3), /* N_S=2 Y=1 G=1 */
      0x01,
      0x02,
      0x03,
      0x04,
      0x05,
      0x06,                             /* 3 resolution pairs */
      2,                                /* N_G=2 */
      (uint8_t)((1u << 3) | (0u << 1)), /* T=1 U=0 R=0 */
      (uint8_t)((0u << 3) | (2u << 1)), /* T=0 U=0 R=2 */
      0x21,
      0x22, /* 2 reference diffs */
      0xAA,
      0xBB /* VP9 bitstream */
  };
  size_t full_len = sizeof(buf) - 2; /* descriptor length */

  sfu_vp9_descriptor_t d;
  assert(sfu_parse_vp9_descriptor(buf, sizeof(buf), &d) == 0);
  assert(d.header_length == full_len);
  assert(d.picture_id == 0x1122);
  assert(d.tid == 2 && d.u_bit == 1 && d.sid == 1 && d.d_bit == 1);
  assert(d.p_diff_count == 3);
  assert(d.p_diff[0] == 1 && d.p_diff[1] == 2 && d.p_diff[2] == 3);

  /* Truncation at every byte boundary of the descriptor must fail. */
  for (size_t cut = 0; cut < full_len; cut++) {
    assert(sfu_parse_vp9_descriptor(buf, cut, &d) == -1);
  }
}

static void test_rejects_bad_input(void) {
  sfu_vp9_descriptor_t d;
  uint8_t buf[] = {VP9_B};
  assert(sfu_parse_vp9_descriptor(NULL, 1, &d) == -1);
  assert(sfu_parse_vp9_descriptor(buf, 0, &d) == -1);
  assert(sfu_parse_vp9_descriptor(buf, 1, NULL) == -1);
  /* Empty descriptor field chain: I=1 but no picture ID byte. */
  uint8_t trunc[] = {VP9_I | VP9_B};
  assert(sfu_parse_vp9_descriptor(trunc, sizeof(trunc), &d) == -1);
}

int main(void) {
  test_minimal_no_extensions();
  test_picture_id_7bit();
  test_picture_id_15bit();
  test_layer_indices_nonflexible_tl0picidx();
  test_layer_indices_flexible_no_tl0picidx();
  test_p_diff_chains();
  test_scalability_structure_minimal();
  test_scalability_structure_resolutions();
  test_scalability_structure_group_of_pictures();
  test_scalability_structure_full();
  test_every_flag_combination();
  test_maximal_descriptor_truncation_sweep();
  test_rejects_bad_input();
  printf("test_vp9_parser: OK\n");
  return 0;
}
