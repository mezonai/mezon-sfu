#include "util/netbytes.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_aligned_roundtrip(void) {
  uint8_t buf[8];

  sfu_write_be16(buf, 0x1234);
  assert(buf[0] == 0x12 && buf[1] == 0x34);
  assert(sfu_read_be16(buf) == 0x1234);

  sfu_write_be32(buf, 0x89ABCDEF);
  assert(buf[0] == 0x89 && buf[1] == 0xAB && buf[2] == 0xCD && buf[3] == 0xEF);
  assert(sfu_read_be32(buf) == 0x89ABCDEFu);

  /* be24 via explicit bytes (no write helper) */
  buf[0] = 0x01;
  buf[1] = 0x02;
  buf[2] = 0x03;
  assert(sfu_read_be24(buf) == 0x00010203u);

  /* edge values */
  sfu_write_be16(buf, 0);
  assert(sfu_read_be16(buf) == 0);
  sfu_write_be16(buf, 0xFFFF);
  assert(sfu_read_be16(buf) == 0xFFFF);

  sfu_write_be32(buf, 0);
  assert(sfu_read_be32(buf) == 0);
  sfu_write_be32(buf, 0xFFFFFFFFu);
  assert(sfu_read_be32(buf) == 0xFFFFFFFFu);

  buf[0] = 0xFF;
  buf[1] = 0xFF;
  buf[2] = 0xFF;
  assert(sfu_read_be24(buf) == 0x00FFFFFFu);
}

static void test_unaligned_reads(void) {
  /* malloc+1 forces odd (typically unaligned) pointer */
  uint8_t *raw = (uint8_t *)malloc(16);
  assert(raw != NULL);
  uint8_t *p = raw + 1;

  memset(raw, 0xCC, 16);

  sfu_write_be16(p, 0xA1B2);
  assert(sfu_read_be16(p) == 0xA1B2);

  sfu_write_be32(p + 2, 0x11223344u);
  assert(sfu_read_be32(p + 2) == 0x11223344u);

  p[6] = 0x0A;
  p[7] = 0x0B;
  p[8] = 0x0C;
  assert(sfu_read_be24(p + 6) == 0x000A0B0Cu);

  /* signed-style large-delta pattern used by TWCC (int16 via be16) */
  sfu_write_be16(p, (uint16_t)(int16_t)-4);
  assert((int16_t)sfu_read_be16(p) == -4);

  free(raw);
}

int main(void) {
  test_aligned_roundtrip();
  test_unaligned_reads();
  printf("test_netbytes: OK\n");
  return 0;
}
