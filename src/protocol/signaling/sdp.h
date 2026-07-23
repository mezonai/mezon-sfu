#ifndef SFU_PROTOCOL_SDP_H
#define SFU_PROTOCOL_SDP_H

#include <stddef.h>
#include <stdint.h>
#include "room/room.h"

int sfu_sdp_build_answer(const char *offer, size_t offer_len, const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint,
                         sfu_publisher_snapshot_t *snaps, uint32_t snaps_count, uint8_t video_pt, uint8_t rtx_pt, char *out, size_t out_cap);

int sfu_sdp_build_offer(const char *host, uint16_t port, const char *ufrag, const char *pwd, const char *fingerprint, sfu_publisher_snapshot_t *snaps,
                        uint32_t snaps_count, uint8_t video_pt, uint8_t rtx_pt, char *out, size_t out_cap);

#endif /* SFU_PROTOCOL_SDP_H */
