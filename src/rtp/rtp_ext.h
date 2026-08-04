#ifndef SFU_RTP_EXT_H
#define SFU_RTP_EXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SFU_TWCC_EXT_LEN 2u

bool sfu_rtp_ext_write_twcc(uint8_t *data, size_t len, size_t cap, uint8_t ext_id, uint16_t twcc_seq, size_t *io_len);

#endif /* SFU_RTP_EXT_H */
