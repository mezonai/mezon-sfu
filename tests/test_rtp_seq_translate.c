#include "rtp/rtp_seq_translate.h"
#include "rtp/rtp_packet.h"
#include "rtp/rtx_build.h"

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

static void test_rtx_same_ssrc_restart(void) {
  const uint8_t original[] = {0x80, 96, 0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 2, 0xaa};
  const uint32_t rtx_ssrc = 0x55667788u;
  sfu_rtp_seq_translator_t translator;
  sfu_rtp_seq_translator_init(&translator);

  uint16_t source_sequences[] = {65000, 65001, 3, 4};
  uint16_t expected_sequences[] = {65000, 65001, 65002, 65003};
  for (size_t i = 0; i < sizeof(source_sequences) / sizeof(source_sequences[0]); i++) {
    uint16_t translated = 0;
    assert(sfu_rtp_seq_translate(&translator, rtx_ssrc, source_sequences[i], &translated));
    assert(translated == expected_sequences[i]);

    uint8_t rtx[sizeof(original) + 2u];
    size_t rtx_len = 0;
    sfu_rtp_packet_t packet;
    assert(sfu_rtx_build(original, sizeof(original), 97, translated, rtx_ssrc, rtx, sizeof(rtx), &rtx_len));
    assert(sfu_rtp_packet_parse(rtx, rtx_len, &packet));
    assert(packet.sequence_number == expected_sequences[i]);
    assert(packet.ssrc == rtx_ssrc);
  }
}

int main(void) {
  test_initial_seed_and_restart();
  test_wrap();
  test_independent_ssrcs();
  test_capacity_failure_preserves_entries();
  test_rtx_same_ssrc_restart();
  printf("test_rtp_seq_translate: OK\n");
  return 0;
}
