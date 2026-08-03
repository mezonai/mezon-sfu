#ifndef SFU_PIPELINE_DISPATCH_H
#define SFU_PIPELINE_DISPATCH_H

#include "runtime/worker.h"
#include "sfu/packet.h"

void sfu_dispatch_packet(sfu_worker_t *w, sfu_packet_t *pkt);

#endif /* SFU_PIPELINE_DISPATCH_H */
