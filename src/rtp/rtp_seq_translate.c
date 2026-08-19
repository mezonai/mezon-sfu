#include "rtp/rtp_seq_translate.h"

#include <string.h>

static uint32_t hash_ssrc(uint32_t ssrc) {
  ssrc ^= ssrc >> 16;
  ssrc *= 0x7feb352du;
  ssrc ^= ssrc >> 15;
  ssrc *= 0x846ca68bu;
  ssrc ^= ssrc >> 16;
  return ssrc;
}

void sfu_rtp_seq_translator_init(sfu_rtp_seq_translator_t *translator) {
  if (translator) {
    memset(translator, 0, sizeof(*translator));
  }
}

bool sfu_rtp_seq_translate(sfu_rtp_seq_translator_t *translator, uint32_t ssrc, uint16_t source_seq, uint16_t *out_seq) {
  if (!translator || !out_seq) {
    return false;
  }

  uint32_t index = hash_ssrc(ssrc) & (SFU_RTP_SEQ_TRANSLATOR_CAP - 1u);
  for (uint32_t probe = 0; probe < SFU_RTP_SEQ_TRANSLATOR_CAP; probe++) {
    sfu_rtp_seq_translation_entry_t *entry = &translator->entries[index];
    if (!entry->valid) {
      entry->ssrc = ssrc;
      entry->next_output_seq = (uint16_t)(source_seq + 1u);
      entry->valid = true;
      *out_seq = source_seq;
      return true;
    }
    if (entry->ssrc == ssrc) {
      *out_seq = entry->next_output_seq;
      entry->next_output_seq = (uint16_t)(entry->next_output_seq + 1u);
      return true;
    }
    index = (index + 1u) & (SFU_RTP_SEQ_TRANSLATOR_CAP - 1u);
  }

  return false;
}
