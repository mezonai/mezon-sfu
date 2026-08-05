#ifndef SFU_PIPELINE_INGRESS_H
#define SFU_PIPELINE_INGRESS_H

#include "pipeline/router.h"
#include "sfu/datadef.h"
#include "sfu/packet.h"

typedef struct sfu_worker sfu_worker_t;

/* Media ingress stage: session lookup -> SRTP unprotect -> RTP/RTCP demux.
 *
 * RTP packets are fully parsed and validated here, SVC metadata is
 * extracted via the codec dispatch, and the result is handed to the
 * packet router. RTCP compounds are iterated member by member and fed to
 * the feedback handlers (TWCC/GCC, NACK/RTX, PLI/FIR).
 *
 * Consumes `pkt` on every path. */
void sfu_ingress_process(sfu_worker_t *w, sfu_packet_t *pkt);

/* CC feedback side-channel used by tests to drive the layer targets the
 * same way the TWCC handler does. */
void sfu_svc_update_layers(sfu_peer_session_t *session, uint32_t bitrate_bps);

#endif /* SFU_PIPELINE_INGRESS_H */
