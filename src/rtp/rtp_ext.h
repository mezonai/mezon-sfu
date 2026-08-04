#ifndef SFU_RTP_EXT_H
#define SFU_RTP_EXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Transport-wide congestion control RTP header extension (CC-01).
 *
 * draft-holmer-rmcat-transport-wide-cc-extensions: the extension payload is a
 * single 16-bit transport-wide sequence number. The writer below supports the
 * RFC 8285 one-byte-header profile (0xBEDE) and the two-byte-header profile
 * (0x1000), which together cover every WebRTC browser.
 *
 * sfu_rtp_ext_write_twcc guarantees the wire packet keeps exactly one
 * transport-cc element for `ext_id`: an existing element with that ID is
 * rewritten in place; otherwise a new element is appended to the extension
 * block (or a new block is created), growing the packet within `cap`. On
 * success *io_len is updated to the new packet length. The packet is
 * unmodified on failure. */

#define SFU_TWCC_EXT_LEN 2u

bool sfu_rtp_ext_write_twcc(uint8_t *data, size_t len, size_t cap, uint8_t ext_id, uint16_t twcc_seq, size_t *io_len);

#endif /* SFU_RTP_EXT_H */
