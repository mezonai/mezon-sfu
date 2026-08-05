#ifndef SFU_PIPELINE_ROUTER_H
#define SFU_PIPELINE_ROUTER_H

#include "media/svc/svc_descriptor.h"
#include "rtp/rtp_packet.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"

typedef struct sfu_worker sfu_worker_t;

/* Parsed-ingress view handed to the router: the media stage below this
 * point only ever sees validated RTP plus its codec-level SVC metadata. */
typedef struct sfu_ingress_media {
  sfu_packet_t *pkt;               /* borrowed; router releases it */
  sfu_rtp_packet_t rtp;            /* parsed header view into pkt->data */
  bool is_audio;                   /* matched the sender's audio uplink PT */
  bool has_svc;                    /* svc descriptor is valid */
  bool is_keyframe;                /* only meaningful when has_svc */
  sfu_svc_descriptor_t svc;        /* codec-agnostic layer info */
} sfu_ingress_media_t;

/* Packet router: resolves the sender's receiver snapshot, applies the
 * per-subscriber layer scheduler, clones the packet, and hands each copy to
 * the owning worker's egress stage (inline when the subscriber is local,
 * through the fanout mesh otherwise).
 *
 * Consumes `m->pkt` on every path. */
void sfu_router_forward(sfu_worker_t *w, sfu_peer_session_t *sender_session, sfu_ingress_media_t *m);

#endif /* SFU_PIPELINE_ROUTER_H */
