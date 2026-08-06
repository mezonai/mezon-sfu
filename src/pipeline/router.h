#ifndef SFU_PIPELINE_ROUTER_H
#define SFU_PIPELINE_ROUTER_H

#include "media/svc/svc_descriptor.h"
#include "rtp/rtp_packet.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"

typedef struct sfu_worker sfu_worker_t;

typedef struct sfu_ingress_media {
  sfu_packet_t *pkt;
  sfu_rtp_packet_t rtp;
  bool is_audio;
  bool has_svc;
  bool is_keyframe;
  sfu_svc_descriptor_t svc;
} sfu_ingress_media_t;

void sfu_router_forward(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m);

#endif /* SFU_PIPELINE_ROUTER_H */
