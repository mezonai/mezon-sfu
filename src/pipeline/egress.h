#ifndef SFU_PIPELINE_EGRESS_H
#define SFU_PIPELINE_EGRESS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "sfu/datadef.h"
#include "sfu/packet.h"

typedef struct sfu_worker sfu_worker_t;

bool sfu_egress_process(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                        uint32_t video_ssrc, uint8_t video_pt, uint8_t video_rtx_pt, uint32_t video_rtx_ssrc, bool has_video, bool is_audio,
                        sfu_pacer_class_t video_class);

#endif /* SFU_PIPELINE_EGRESS_H */
