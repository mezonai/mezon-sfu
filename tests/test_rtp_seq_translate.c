#include "rtp/rtp_seq_translate.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_initial_seed_and_restart(void) {
  sfu_rtp_seq_translator_t translator;
  sfu_rtp_seq_translator_init(&translator);

  uint16_t seq = 0;
  assert(sfu_rtp_seq_translate(&translator, 0x11223344u, 100, &seq) && seq == 100);
  assert(sfu_rtp_seq_translate(&translator, 0x11223344u, 102, &seq) && seq == 101);
  assert(sfu_rtp_seq_translate(&translator, 0x11223344u, 4, &seq) && seq == 102);
  assert(sfu_rtp_seq_translate(&translator, 0x11223344u, 4, &seq) && seq == 103);
}

static void test_wrap(void) {
  sfu_rtp_seq_translator_t translator;
  sfu_rtp_seq_translator_init(&translator);

  uint16_t seq = 0;
  assert(sfu_rtp_seq_translate(&translator, 7, UINT16_MAX, &seq) && seq == UINT16_MAX);
  assert(sfu_rtp_seq_translate(&translator, 7, 10, &seq) && seq == 0);
  assert(sfu_rtp_seq_translate(&translator, 7, 11, &seq) && seq == 1);
}

static void test_independent_ssrcs(void) {
  sfu_rtp_seq_translator_t translator;
  sfu_rtp_seq_translator_init(&translator);

  uint16_t seq = 0;
  assert(sfu_rtp_seq_translate(&translator, 10, 500, &seq) && seq == 500);
  assert(sfu_rtp_seq_translate(&translator, 20, 900, &seq) && seq == 900);
  assert(sfu_rtp_seq_translate(&translator, 10, 1, &seq) && seq == 501);
  assert(sfu_rtp_seq_translate(&translator, 20, 2, &seq) && seq == 901);
  assert(sfu_rtp_seq_translate(&translator, 0, 77, &seq) && seq == 77);
  assert(sfu_rtp_seq_translate(&translator, 0, 1, &seq) && seq == 78);
}

static void test_capacity_failure_preserves_entries(void) {
  sfu_rtp_seq_translator_t translator;
  sfu_rtp_seq_translator_init(&translator);

  uint16_t seq = 0;
  for (uint32_t i = 0; i < SFU_RTP_SEQ_TRANSLATOR_CAP; i++) {
    uint32_t ssrc = i + 1u;
    assert(sfu_rtp_seq_translate(&translator, ssrc, (uint16_t)i, &seq));
    assert(seq == (uint16_t)i);
  }

  assert(!sfu_rtp_seq_translate(&translator, SFU_RTP_SEQ_TRANSLATOR_CAP + 1u, 55, &seq));
  assert(sfu_rtp_seq_translate(&translator, 1, 0, &seq));
  assert(seq == 1);
}

int main(void) {
  test_initial_seed_and_restart();
  test_wrap();
  test_independent_ssrcs();
  test_capacity_failure_preserves_entries();
  printf("test_rtp_seq_translate: OK\n");
  return 0;
}
