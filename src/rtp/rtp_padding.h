#ifndef SFU_RTP_PADDING_H
#define SFU_RTP_PADDING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_RTP_PADDING_DEFAULT_BYTES 224u
#define SFU_RTP_PADDING_MAX_BYTES 255u

/*
 * Builds a standards-compliant RTP padding packet (RFC 3550 §5.1, RFC 8285).
 *
 * Parameters:
 *   out: destination buffer for the packet
 *   out_cap: size of the destination buffer
 *   payload_type: RTP payload type (7 bits, e.g. negotiated RTX PT)
 *   seq: 16-bit RTP sequence number
 *   timestamp: 32-bit RTP timestamp
 *   ssrc: 32-bit RTP SSRC (e.g. negotiated RTX SSRC)
 *   padding_bytes: total padding bytes (1-255; if 0, defaults to SFU_RTP_PADDING_DEFAULT_BYTES)
 *   twcc_ext_id: 1-byte TWCC extension ID (1-14); if 0, TWCC extension is omitted
 *   twcc_seq: 16-bit TWCC transport sequence number
 *   out_len: on success, receives the total wire length of the built packet
 *
 * Returns true on success, false if parameters are invalid or out_cap is insufficient.
 */
bool sfu_rtp_padding_build(uint8_t *out, size_t out_cap, uint8_t payload_type,
                           uint16_t seq, uint32_t timestamp, uint32_t ssrc,
                           uint16_t padding_bytes, uint8_t twcc_ext_id,
                           uint16_t twcc_seq, size_t *out_len);

#endif /* SFU_RTP_PADDING_H */
