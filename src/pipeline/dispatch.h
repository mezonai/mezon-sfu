#ifndef SFU_PIPELINE_DISPATCH_H
#define SFU_PIPELINE_DISPATCH_H

#include "runtime/worker.h"
#include "sfu/packet.h"

/*
 * Every datagram arriving on the shared UDP port is one of three things
 * (RFC 7983, distinguished by the first byte): a STUN connectivity
 * check, a DTLS handshake/record, or SRTP-protected RTP/RTCP media.
 * This is the single point where that split happens, sitting between
 * "a worker popped a packet off its inbox" and "the room-forward path
 * decides who else should see it" -- STUN and DTLS packets terminate
 * here (they get a direct reply, never fan out to the room); only
 * RTP/RTCP for an established session reaches sfu_room_forward_packet.
 *
 * Consumes exactly one reference on pkt, same contract every packet
 * handler in this codebase follows.
 */
void sfu_dispatch_packet(sfu_worker_t *w, sfu_packet_t *pkt);

#endif /* SFU_PIPELINE_DISPATCH_H */
