/* libFuzzer entry point for the wire parsers (#86).
 *
 * One harness, five parsers, dispatched on the first byte of input so a
 * single corpus serves all of them (the standard "fuzz target router"
 * pattern; keeps `make fuzz` to one binary):
 *
 *   0 -> compound RTCP iterator
 *   1 -> TWCC feedback parser (drives the estimator-facing contract)
 *   2 -> NACK FCI parser
 *   3 -> RTP header-extension TWCC writer (mutable buffer, growth path)
 *   4 -> VP9 payload descriptor parser
 *
 * Build (requires clang with -fsanitize=fuzzer):
 *   clang -fsanitize=fuzzer,address,undefined -I include -I src \
 *       tests/fuzz/fuzz_parsers.c <parser objects> -o fuzz_parsers
 *
 * The parsers are pure (no I/O, no allocation beyond the caller's stack),
 * so this target is safe to run at full speed for days. Any crash input is
 * a security bug: every byte here is attacker-controlled off the wire.
 *
 * This file is intentionally NOT wired into the default CTest build — it
 * needs clang + libFuzzer, which CI provides in the sanitizer job. It is
 * compiled by tests/fuzz/build_fuzz.sh. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "congestion/twcc_parser.h"
#include "media/svc/vp9_parser.h"
#include "rtcp/rtcp_compound.h"
#include "rtp/rtp_ext.h"
#include "rtp/rtx.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 2) {
    return 0;
  }
  uint8_t which = data[0] % 5;
  const uint8_t *d = data + 1;
  size_t len = size - 1;

  switch (which) {
    case 0: {
      sfu_rtcp_compound_iter it;
      sfu_rtcp_compound_iter_init(&it, d, len);
      sfu_rtcp_member_view view;
      size_t guard = 0;
      while (sfu_rtcp_compound_iter_next(&it, &view) == SFU_RTCP_COMPOUND_ITEM) {
        if (++guard > 4096) {
          __builtin_trap(); /* iterator must terminate on its own */
        }
      }
      break;
    }
    case 1: {
      sfu_twcc_parser_t p;
      if (sfu_twcc_parser_init(&p, d, len, 0) != 0) {
        break;
      }
      gcc_packet_info_t item;
      size_t guard = 0;
      while (sfu_twcc_parser_next(&p, &item)) {
        if (++guard > 65536) {
          __builtin_trap();
        }
      }
      break;
    }
    case 2: {
      sfu_nack_parser_t p;
      if (!sfu_nack_parser_init(&p, d, len)) {
        break;
      }
      uint16_t seq;
      size_t guard = 0;
      while (sfu_nack_parser_next(&p, &seq)) {
        if (++guard > 65536) {
          __builtin_trap();
        }
      }
      break;
    }
    case 3: {
      /* The writer grows the buffer in place; give it full capacity. */
      if (len > 1024) {
        break;
      }
      uint8_t buf[2048];
      memcpy(buf, d, len);
      size_t io_len = len;
      (void)sfu_rtp_ext_write_twcc(buf, len, sizeof(buf), 5, 0x1234, &io_len);
      break;
    }
    case 4: {
      sfu_vp9_descriptor_t desc;
      (void)sfu_parse_vp9_descriptor(d, len, &desc);
      break;
    }
  }
  return 0;
}
