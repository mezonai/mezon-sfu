#include "net/socket.h"
#include "util/log.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SFU_SO_RCVBUF_BYTES (8 * 1024 * 1024)
#define SFU_SO_SNDBUF_BYTES (8 * 1024 * 1024)

int sfu_udp_socket_create(uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
  if (fd < 0) {
    SFU_LOG_ERROR("socket() failed: %s", strerror(errno));
    return -1;
  }

  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
    SFU_LOG_ERROR("SO_REUSEPORT failed: %s", strerror(errno));
    close(fd);
    return -1;
  }
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    SFU_LOG_WARN("SO_REUSEADDR failed: %s", strerror(errno));
  }

  int rcvbuf = SFU_SO_RCVBUF_BYTES;
  int sndbuf = SFU_SO_SNDBUF_BYTES;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
    SFU_LOG_WARN("SO_RCVBUF failed: %s", strerror(errno));
  }
  if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
    SFU_LOG_WARN("SO_SNDBUF failed: %s", strerror(errno));
  }
  if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)) < 0) {
    SFU_LOG_WARN("SO_TIMESTAMPNS failed: %s", strerror(errno));
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    SFU_LOG_ERROR("bind() to port %u failed: %s", port, strerror(errno));
    close(fd);
    return -1;
  }

  SFU_LOG_INFO("UDP socket bound on port %u (fd=%d)", port, fd);
  return fd;
}
