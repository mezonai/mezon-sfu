#include "rtp/rtx.h"
#include "rtp/rtx_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sfu/datadef.h"

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      return 1;                                                            \
    }                                                                      \
  } while (0)

/*
 * RTX capacity / init-hardening tests (P0.5).
 *
 * No project-wide failing-allocator hook exists, so the OOM branch in
 * sfu_rtx_cache_init cannot be forced from this binary. Instead we:
 *   1. Verify put/get size gate (room for 2-byte RTX OSN):
 *        len == SFU_MAX_PAYLOAD_SIZE - 2  -> accepted
 *        len == SFU_MAX_PAYLOAD_SIZE - 1  -> rejected
 *        len == SFU_MAX_PAYLOAD_SIZE      -> rejected
 *   2. Verify destroy is safe on a zeroed cache (the state left after an
 *      init-failure cleanup path frees already-allocated entry buffers).
 */
int main(void) {
  /* --- init-failure leftover: zeroed cache must be destroy-safe --- */
  {
    sfu_rtx_cache_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    sfu_rtx_cache_destroy(&zeroed); /* must not crash / free garbage */
  }

  /* --- happy-path init --- */
  sfu_rtx_cache_t cache;
  EXPECT(sfu_rtx_cache_init(&cache) == 0);

  uint8_t *buf = (uint8_t *)malloc(SFU_MAX_PAYLOAD_SIZE);
  EXPECT(buf != NULL);
  memset(buf, 0xA5, SFU_MAX_PAYLOAD_SIZE);

  uint8_t out[SFU_MAX_PAYLOAD_SIZE];
  uint32_t out_len = 0;
  uint32_t out_ssrc = 0;
  uint8_t out_pt = 0;

  /* Boundary: max cacheable length leaves 2 bytes for RTX OSN. */
  const uint32_t ok_len = SFU_MAX_PAYLOAD_SIZE - 2;
  sfu_rtx_cache_put(&cache, 100, buf, ok_len, 0x11223344u, 96);
  EXPECT(sfu_rtx_cache_get(&cache, 100, out, &out_len, &out_ssrc, &out_pt));
  EXPECT(out_len == ok_len);
  EXPECT(out_ssrc == 0x11223344u);
  EXPECT(out_pt == 96);
  EXPECT(memcmp(out, buf, ok_len) == 0);

  /* One byte over the safe expand limit: must not be cached. */
  sfu_rtx_cache_put(&cache, 200, buf, SFU_MAX_PAYLOAD_SIZE - 1, 1u, 97);
  EXPECT(!sfu_rtx_cache_get(&cache, 200, out, &out_len, &out_ssrc, &out_pt));

  /* Full payload size: must not be cached. */
  sfu_rtx_cache_put(&cache, 300, buf, SFU_MAX_PAYLOAD_SIZE, 2u, 98);
  EXPECT(!sfu_rtx_cache_get(&cache, 300, out, &out_len, &out_ssrc, &out_pt));

  /* Earlier accepted entry is still intact. */
  EXPECT(sfu_rtx_cache_get(&cache, 100, out, &out_len, &out_ssrc, &out_pt));
  EXPECT(out_len == ok_len);

  free(buf);
  sfu_rtx_cache_destroy(&cache);

  /* Double-destroy after destroy nulls entry pointers must be safe. */
  sfu_rtx_cache_destroy(&cache);

  /* --- wire boundary (#86): a cached packet at the maximum cacheable
   * length (SFU_MAX_PAYLOAD_SIZE - 2) must round-trip through
   * sfu_rtx_build into a pool-sized buffer (SFU_MAX_PAYLOAD_SIZE) exactly,
   * and must fail cleanly with one byte less. --- */
  {
    sfu_rtx_cache_t c2;
    EXPECT(sfu_rtx_cache_init(&c2) == 0);

    /* Minimal RTP header (12 bytes) + payload to reach the boundary. */
    uint8_t *wire = (uint8_t *)malloc(SFU_MAX_PAYLOAD_SIZE);
    EXPECT(wire != NULL);
    wire[0] = 0x80;
    wire[1] = 96;
    wire[2] = 0;
    wire[3] = 7; /* seq 7 */
    memset(wire + 4, 0, 8);         /* timestamp + ssrc */
    memset(wire + 12, 0x5a, SFU_MAX_PAYLOAD_SIZE - 12);

    const uint32_t max_cached = SFU_MAX_PAYLOAD_SIZE - 2;
    sfu_rtx_cache_put_stream(&c2, 7, wire, max_cached, 0x11223344u, 97, 0xaabbccddu, 0);

    uint8_t *orig = (uint8_t *)malloc(SFU_MAX_PAYLOAD_SIZE);
    EXPECT(orig != NULL);
    uint32_t orig_len = 0;
    uint32_t rtx_ssrc = 0;
    uint8_t rtx_pt = 0;
    EXPECT(sfu_rtx_cache_get_stream(&c2, 7, orig, &orig_len, &rtx_ssrc, &rtx_pt, 0xaabbccddu, 0));
    EXPECT(orig_len == max_cached);

    uint8_t *outb = (uint8_t *)malloc(SFU_MAX_PAYLOAD_SIZE + 1);
    EXPECT(outb != NULL);
    memset(outb, 0xa5, SFU_MAX_PAYLOAD_SIZE + 1);
    size_t built = 0;
    /* orig_len + 2 == SFU_MAX_PAYLOAD_SIZE == pool buffer cap: fits exactly. */
    EXPECT(sfu_rtx_build(orig, orig_len, rtx_pt, 9000, rtx_ssrc, outb, SFU_MAX_PAYLOAD_SIZE, &built));
    EXPECT(built == SFU_MAX_PAYLOAD_SIZE);
    /* One byte less must fail without touching the buffer. Restore the
     * canary first (the successful build above already wrote there). */
    memset(outb, 0xa5, SFU_MAX_PAYLOAD_SIZE + 1);
    size_t built2 = 777;
    EXPECT(!sfu_rtx_build(orig, orig_len, rtx_pt, 9000, rtx_ssrc, outb, SFU_MAX_PAYLOAD_SIZE - 1, &built2));
    EXPECT(built2 == 777);                          /* untouched on failure */
    EXPECT(outb[0] == 0xa5 && outb[SFU_MAX_PAYLOAD_SIZE] == 0xa5);

    free(outb);
    free(orig);
    free(wire);
    sfu_rtx_cache_destroy(&c2);
  }

  printf("test_rtx_capacity: OK\n");
  return 0;
}
