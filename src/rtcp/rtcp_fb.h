#ifndef SFU_RTCP_FB_H
#define SFU_RTCP_FB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtcp/rtcp_compound.h"

typedef struct {
  uint32_t sender_ssrc;
  uint32_t media_ssrc;
} sfu_rtcp_pli;

typedef struct {
  uint32_t sender_ssrc;
  uint32_t media_ssrc;
  const uint8_t *fci;
  size_t entry_count;
} sfu_rtcp_fir;

typedef struct {
  uint32_t target_ssrc;
  uint8_t sequence_number;
} sfu_rtcp_fir_entry;

bool sfu_rtcp_parse_pli(const sfu_rtcp_member_view *member, sfu_rtcp_pli *pli);
bool sfu_rtcp_parse_fir(const sfu_rtcp_member_view *member, sfu_rtcp_fir *fir);
bool sfu_rtcp_fir_entry_at(const sfu_rtcp_fir *fir, size_t index,
                           sfu_rtcp_fir_entry *entry);

#endif /* SFU_RTCP_FB_H */
