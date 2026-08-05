#ifndef SFU_NET_ZEROCOPY_H
#define SFU_NET_ZEROCOPY_H

#include <stddef.h>
#include <sys/socket.h>

#include "net/io_uring.h"
#include "sfu/packet.h"

size_t sfu_fanout_send_zc(sfu_ring_t *ring, sfu_packet_t *pkt, const struct sockaddr_storage *dsts, const socklen_t *dst_lens, size_t count);

#endif /* SFU_NET_ZEROCOPY_H */
