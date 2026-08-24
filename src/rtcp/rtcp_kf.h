#ifndef SFU_RTCP_KF
#define SFU_RTCP_KF

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>

int sfu_rtcp_build_pli(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *buffer, size_t cap);
int sfu_rtcp_build_fir(uint32_t sender_ssrc, uint32_t media_ssrc, uint8_t *fir_seq, uint8_t *buffer, size_t cap);
int sfu_rtcp_build_remb(uint32_t sender_ssrc, uint64_t bitrate_bps, const uint32_t *media_ssrcs, uint8_t media_ssrc_count, uint8_t *buffer,
                        size_t cap);

#endif  // SFU_RTCP_KF
