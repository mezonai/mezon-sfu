#ifndef SFU_PIPELINE_INGRESS_H
#define SFU_PIPELINE_INGRESS_H

#include "pipeline/router.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"

typedef struct sfu_worker sfu_worker_t;

void sfu_ingress_process(sfu_worker_t *w, sfu_packet_t *pkt);
void sfu_svc_update_layers(sfu_peer_session_t *session, uint32_t bitrate_bps);

#endif /* SFU_PIPELINE_INGRESS_H */
