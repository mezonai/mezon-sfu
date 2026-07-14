#ifndef SFU_NET_ZEROCOPY_H
#define SFU_NET_ZEROCOPY_H

#include <stddef.h>
#include <sys/socket.h>

#include "sfu/packet.h"
#include "net/io_uring.h"

/*
 * Fans one packet out to N destinations without copying payload bytes.
 * This is the SFU's core forwarding primitive: a publisher's packet gets
 * queued once per subscriber via IORING_OP_SEND_ZC, all sharing the same
 * underlying buffer.
 *
 * Refcount discipline: sfu_ring_queue_send_zc() retains the packet once
 * per call *before* querying/submitting, so by the time this loop
 * finishes queuing all N sends, the packet is guaranteed to have at
 * least N+1 references (N in-flight sends + the caller's own reference)
 * -- it can never transiently hit zero mid-fanout.
 *
 * Caller still owns its own reference to `pkt` after this returns and
 * must release it exactly once (e.g. via sfu_ring_release_packet) when
 * done -- this function does not consume the caller's reference.
 *
 * Returns the number of destinations successfully queued; if this is
 * less than `count`, the caller's SQ was full for the remainder -- flush
 * with sfu_ring_submit() and retry the rest, or drop them (both are
 * legitimate under backpressure; dropping trades fairness for latency).
 */
size_t sfu_fanout_send_zc(sfu_ring_t *ring, sfu_packet_t *pkt,
                           const struct sockaddr_storage *dsts,
                           const socklen_t *dst_lens, size_t count);

#endif /* SFU_NET_ZEROCOPY_H */
