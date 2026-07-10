#ifndef SFU_NET_SOCKET_H
#define SFU_NET_SOCKET_H

#include <stdint.h>

/*
 * Creates a non-blocking UDP socket bound to `port` on all interfaces,
 * with SO_REUSEPORT set and generously sized recv/send buffers (media
 * traffic bursts hard on keyframes/PLI-triggered retransmits).
 *
 * Returns the fd, or -1 on error (errno set).
 */
int sfu_udp_socket_create(uint16_t port);

#endif /* SFU_NET_SOCKET_H */
