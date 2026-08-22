#ifndef SFU_PACKET_H
#define SFU_PACKET_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef enum sfu_buf_source {
  SFU_BUF_SOURCE_POOL = 0,
  SFU_BUF_SOURCE_KERNEL = 1,
  SFU_BUF_SOURCE_WORKER_ARENA = 2,
} sfu_buf_source_t;

typedef struct sfu_packet {
  uint8_t *data;
  uint32_t len;
  uint32_t cap;
  _Atomic uint32_t refcount;
  _Atomic uint32_t generation;
  uint32_t pool_index;
  uint32_t kbuf_index;
  uint8_t buf_source;
  void *buf_owner;
  struct sockaddr_storage peer_addr;
  socklen_t peer_addr_len;
  uint64_t recv_ts_ns;
} sfu_packet_t;

#endif /* SFU_PACKET_H */
