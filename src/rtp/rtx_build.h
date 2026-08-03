#ifndef SFU_RTP_RTX_BUILD_H
#define SFU_RTP_RTX_BUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Build a plaintext RFC 4588 RTX packet in out.
 *
 * The complete original RTP header (including CSRCs and header extension) is
 * preserved, then PT, sequence number and SSRC are replaced with the RTX
 * values. The original sequence number (OSN) is prepended to the media
 * payload. Original RTP padding is deliberately stripped and the output P bit
 * is cleared; padding bytes are not part of the retransmitted payload.
 *
 * On failure neither out nor *out_len is modified. orig and out must refer to
 * non-overlapping storage.
 */
bool sfu_rtx_build(const uint8_t *orig, size_t orig_len, uint8_t rtx_pt,
                   uint16_t rtx_seq, uint32_t rtx_ssrc, uint8_t *out,
                   size_t out_cap, size_t *out_len);

#endif /* SFU_RTP_RTX_BUILD_H */
