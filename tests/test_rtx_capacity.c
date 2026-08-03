#include "rtp/rtx.h"

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

  printf("test_rtx_capacity: OK\n");
  return 0;
}
