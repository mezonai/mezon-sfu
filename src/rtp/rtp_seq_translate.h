#ifndef SFU_RTP_SEQ_TRANSLATE_H
#define SFU_RTP_SEQ_TRANSLATE_H

#include <stdbool.h>
#include <stdint.h>

#define SFU_RTP_SEQ_TRANSLATOR_CAP 2048u

typedef struct sfu_rtp_seq_translation_entry {
  uint32_t ssrc;
  uint16_t next_output_seq;
  bool valid;
} sfu_rtp_seq_translation_entry_t;

typedef struct sfu_rtp_seq_translator {
  sfu_rtp_seq_translation_entry_t entries[SFU_RTP_SEQ_TRANSLATOR_CAP];
} sfu_rtp_seq_translator_t;

void sfu_rtp_seq_translator_init(sfu_rtp_seq_translator_t *translator);
bool sfu_rtp_seq_translate(sfu_rtp_seq_translator_t *translator, uint32_t ssrc, uint16_t source_seq, uint16_t *out_seq);

#endif /* SFU_RTP_SEQ_TRANSLATE_H */
