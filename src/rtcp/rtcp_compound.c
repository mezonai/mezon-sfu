#include "rtcp/rtcp_compound.h"

#include <string.h>

#include "util/netbytes.h"

void sfu_rtcp_compound_iter_init(sfu_rtcp_compound_iter *iter, const uint8_t *packet,
                                 size_t packet_len) {
  iter->next = packet;
  iter->remaining = packet_len;
  iter->malformed = 0;
}

static sfu_rtcp_compound_result malformed(sfu_rtcp_compound_iter *iter) {
  iter->malformed = 1;
  iter->remaining = 0;
  return SFU_RTCP_COMPOUND_MALFORMED;
}

sfu_rtcp_compound_result sfu_rtcp_compound_iter_next(sfu_rtcp_compound_iter *iter,
                                                     sfu_rtcp_member_view *view) {
  if (iter == NULL || view == NULL) return SFU_RTCP_COMPOUND_MALFORMED;
  if (iter->malformed) return SFU_RTCP_COMPOUND_MALFORMED;
  if (iter->remaining == 0) return SFU_RTCP_COMPOUND_END;
  if (iter->next == NULL || iter->remaining < 4) return malformed(iter);

  const uint8_t *member = iter->next;
  const uint8_t first = member[0];
  if ((first >> 6) != 2) return malformed(iter);

  const uint16_t words_minus_one = sfu_read_be16(member + 2);
  const size_t member_bytes = ((size_t)words_minus_one + 1u) * 4u;
  if (member_bytes < 8 || member_bytes > iter->remaining) return malformed(iter);

  size_t logical_bytes = member_bytes;
  if ((first & 0x20u) != 0) {
    const uint8_t padding = member[member_bytes - 1];
    const size_t payload_bytes = member_bytes - 4;
    if (padding == 0 || (size_t)padding > payload_bytes) return malformed(iter);
    logical_bytes -= padding;
    if (logical_bytes < 8) return malformed(iter);
  }

  memset(view, 0, sizeof(*view));
  view->member = member;
  view->member_len = logical_bytes;
  view->fmt_count = first & 0x1fu;
  view->pt = member[1];
  view->sender_ssrc = sfu_read_be32(member + 4);
  view->body = member + 8;
  view->body_len = logical_bytes - 8;

  iter->next += member_bytes;
  iter->remaining -= member_bytes;
  return SFU_RTCP_COMPOUND_ITEM;
}
