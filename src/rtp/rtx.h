#ifndef SFU_RTX_H
#define SFU_RTX_H

#include <stdbool.h>
#include <stdint.h>

#define SFU_RTX_CACHE_SIZE 1024
#define SFU_RTX_CACHE_MASK (SFU_RTX_CACHE_SIZE - 1)

typedef struct sfu_rtx_entry {
  uint8_t *data;
  uint32_t len;
  uint32_t rtx_ssrc;
  uint8_t rtx_pt;
  uint16_t seq;
  bool valid;
} sfu_rtx_entry_t;

typedef struct sfu_rtx_cache {
  sfu_rtx_entry_t entries[SFU_RTX_CACHE_SIZE];
  uint16_t next_rtx_seq;
} sfu_rtx_cache_t;

typedef struct {
  const uint8_t *nack_ptr;
  const uint8_t *nack_end;
  uint16_t current_pid;
  uint16_t current_blp;
  int bit_index;
} sfu_nack_parser_t;

void sfu_rtx_cache_init(sfu_rtx_cache_t *cache);
void sfu_rtx_cache_put(sfu_rtx_cache_t *cache, uint16_t seq, const uint8_t *data, uint32_t len, uint32_t rtx_ssrc, uint8_t rtx_pt);
bool sfu_rtx_cache_get(sfu_rtx_cache_t *cache, uint16_t seq, uint8_t *out_data, uint32_t *out_len, uint32_t *out_rtx_ssrc, uint8_t *out_rtx_pt);

void sfu_nack_parser_init(sfu_nack_parser_t *parser, const uint8_t *data, uint32_t len);
bool sfu_nack_parser_next(sfu_nack_parser_t *parser, uint16_t *lost_seq);

#endif  // SFU_RTX_H
