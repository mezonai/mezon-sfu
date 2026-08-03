#include "rtcp/rtcp_fb.h"

#include "util/netbytes.h"

bool sfu_rtcp_parse_pli(const sfu_rtcp_member_view *member, sfu_rtcp_pli *pli) {
  if (member == NULL || pli == NULL || member->member == NULL || member->body == NULL)
    return false;
  if (member->pt != 206 || member->fmt_count != 1 || member->member_len != 12 ||
      member->body_len != 4 || member->body != member->member + 8)
    return false;

  pli->sender_ssrc = member->sender_ssrc;
  pli->media_ssrc = sfu_read_be32(member->body);
  return true;
}

bool sfu_rtcp_parse_fir(const sfu_rtcp_member_view *member, sfu_rtcp_fir *fir) {
  if (member == NULL || fir == NULL || member->member == NULL || member->body == NULL)
    return false;
  if (member->pt != 206 || member->fmt_count != 4 || member->member_len < 20 ||
      member->body_len < 12 || member->body != member->member + 8 ||
      member->member_len != member->body_len + 8 ||
      (member->body_len - 4) % 8 != 0)
    return false;

  const size_t count = (member->body_len - 4) / 8;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t *entry = member->body + 4 + i * 8;
    if (entry[5] != 0 || entry[6] != 0 || entry[7] != 0) return false;
  }

  fir->sender_ssrc = member->sender_ssrc;
  fir->media_ssrc = sfu_read_be32(member->body);
  fir->fci = member->body + 4;
  fir->entry_count = count;
  return true;
}

bool sfu_rtcp_fir_entry_at(const sfu_rtcp_fir *fir, size_t index,
                           sfu_rtcp_fir_entry *entry) {
  if (fir == NULL || entry == NULL || fir->fci == NULL || index >= fir->entry_count)
    return false;
  const uint8_t *p = fir->fci + index * 8;
  entry->target_ssrc = sfu_read_be32(p);
  entry->sequence_number = p[4];
  return true;
}
