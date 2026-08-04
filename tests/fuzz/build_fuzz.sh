#!/bin/sh
# Build the libFuzzer parser harness (#86). Requires clang with the fuzzer
# sanitizer; run in CI's sanitizer job or locally with clang installed.
#
# Usage: tests/fuzz/build_fuzz.sh [output-dir]
# Then run:  <out>/fuzz_parsers -max_total_time=600 corpus/ artifacts/
set -eu

OUT="${1:-build-fuzz}"
mkdir -p "$OUT"

CFLAGS="-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1 -I include -I src"

# Only the pure parser translation units — no I/O, no threading, no uring.
SRCS="
  src/congestion/twcc_parser.c
  src/congestion/gcc.c
  src/media/svc/vp9_parser.c
  src/rtcp/rtcp_compound.c
  src/rtcp/rtcp_fb.c
  src/rtp/rtp_ext.c
  src/rtp/rtp_packet.c
  src/rtp/rtx.c
  src/rtp/rtx_build.c
  tests/fuzz/fuzz_parsers.c
"

# shellcheck disable=SC2086
clang $CFLAGS $SRCS -lm -o "$OUT/fuzz_parsers"
echo "built $OUT/fuzz_parsers"
