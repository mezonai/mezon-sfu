#ifndef SFU_RTP_EXT_H
#define SFU_RTP_EXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_TWCC_EXT_LEN 2u

bool sfu_rtp_ext_write_twcc(uint8_t *data, size_t len, size_t cap, uint8_t ext_id, uint16_t twcc_seq, size_t *io_len);
bool sfu_rtp_ext_read_twcc(uint16_t extension_profile, const uint8_t *ext, size_t ext_len, uint8_t ext_id, uint16_t *out_seq);
bool sfu_rtp_ext_read_mid(uint16_t extension_profile, const uint8_t *ext, size_t ext_len, uint8_t ext_id, char *out_mid, size_t out_cap);

#endif /* SFU_RTP_EXT_H */
