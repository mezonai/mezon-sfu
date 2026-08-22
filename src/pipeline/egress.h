#ifndef SFU_PIPELINE_EGRESS_H
#define SFU_PIPELINE_EGRESS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "media/svc/svc_descriptor.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"

typedef struct sfu_worker sfu_worker_t;

typedef struct sfu_egress_media {
  sfu_peer_session_t *publisher;
  sfu_svc_descriptor_t svc;
  sfu_media_kind_t source;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint32_t mid;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  bool has_video;
  bool is_audio;
  bool has_svc;
  bool is_keyframe;
} sfu_egress_media_t;

bool sfu_egress_process(sfu_worker_t *w, sfu_peer_session_t *sub_session, sfu_packet_t *pkt, const struct sockaddr_storage *dst, socklen_t dst_len,
                        const sfu_egress_media_t *media);
bool sfu_egress_process_plaintext(sfu_worker_t *w, sfu_peer_session_t *sub_session, const sfu_packet_t *plain, const struct sockaddr_storage *dst,
                                  socklen_t dst_len, const sfu_egress_media_t *media);
bool sfu_egress_process_plaintext_reserved(sfu_worker_t *w, sfu_peer_session_t *sub_session, const sfu_packet_t *plain, sfu_packet_t *output,
                                           const struct sockaddr_storage *dst, socklen_t dst_len, const sfu_egress_media_t *media);

#endif /* SFU_PIPELINE_EGRESS_H */
