#ifndef SFU_RTCP_COMPOUND_H
#define SFU_RTCP_COMPOUND_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  SFU_RTCP_COMPOUND_MALFORMED = -1,
  SFU_RTCP_COMPOUND_END = 0,
  SFU_RTCP_COMPOUND_ITEM = 1,
} sfu_rtcp_compound_result;

typedef struct {
  const uint8_t *member;
  size_t member_len;
  uint8_t fmt_count;
  uint8_t pt;
  uint32_t sender_ssrc;
  const uint8_t *body;
  size_t body_len;
} sfu_rtcp_member_view;

typedef struct {
  const uint8_t *next;
  size_t remaining;
  int malformed;
} sfu_rtcp_compound_iter;

void sfu_rtcp_compound_iter_init(sfu_rtcp_compound_iter *iter, const uint8_t *packet,
                                 size_t packet_len);

/*
 * Members must be at least eight bytes so sender_ssrc is always available.
 * member_len and body_len exclude any RTCP padding; member points at the exact
 * bounded member and body starts immediately after the sender SSRC.
 */
sfu_rtcp_compound_result sfu_rtcp_compound_iter_next(sfu_rtcp_compound_iter *iter,
                                                     sfu_rtcp_member_view *view);

#endif /* SFU_RTCP_COMPOUND_H */
