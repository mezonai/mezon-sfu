#ifndef SFU_NET_IO_BACKEND_H
#define SFU_NET_IO_BACKEND_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "memory/packet_pool.h"
#include "sfu/packet.h"
#include "util/ringbuffer.h"

#define SFU_RECV_CMSG_BUFSIZE 64u

#ifdef USE_AF_XDP

typedef struct sfu_ring {
  sfu_spsc_ring_t tx_pending;
  sfu_spsc_ring_t tx_completed;
  _Atomic uint32_t outstanding_sends;
  uint32_t queue_capacity;
  int fd;
  bool with_recv_bufs;
  bool queues_initialized;
} sfu_ring_t;

#else

#include <liburing.h>

typedef struct sfu_ring {
  struct io_uring ring;
  struct msghdr recv_msg_template;
  uint8_t recv_cmsg_buf[SFU_RECV_CMSG_BUFSIZE];
  struct io_uring_buf_ring *buf_ring;
  void *buf_ring_mem;
  uint32_t buf_count;
  uint32_t buf_size;
  uint32_t payload_cap;
  _Atomic uint32_t outstanding_sends;
  int bgid;
  int fd;
} sfu_ring_t;

#define SFU_CQE_TAG_RECV 0x1ULL
#define SFU_CQE_TAG_MASK 0x1ULL

#endif

typedef void (*sfu_on_recv_fn)(void *user_data, sfu_packet_t *pkt);
typedef void (*sfu_on_send_complete_fn)(void *user_data, sfu_packet_t *pkt);

uint32_t sfu_ring_recv_overhead(void);
uint32_t sfu_ring_recv_slot_size(uint32_t payload_cap);

int sfu_ring_backend_init(int fd, const char *interface_name, uint32_t queue_id, uint16_t media_port, uint32_t frame_count, uint32_t frame_size,
                          const char *xdp_mode);
void sfu_ring_backend_destroy(void);
unsigned sfu_ring_backend_service(sfu_ring_t *recv_ring, sfu_ring_t *send_rings, uint32_t send_ring_count, unsigned max_count);

int sfu_ring_init(sfu_ring_t *r, int fd, uint32_t sq_entries, uint32_t cq_entries, uint32_t buf_count, uint32_t buf_size, int bgid, bool with_recv_bufs);
void sfu_ring_destroy(sfu_ring_t *r);
int sfu_ring_arm_recv(sfu_ring_t *r);
int sfu_ring_queue_send_zc(sfu_ring_t *r, sfu_packet_t *pkt, const struct sockaddr *dst, socklen_t dst_len);
int sfu_ring_submit(sfu_ring_t *r);
unsigned sfu_ring_reap(sfu_ring_t *r, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher, sfu_on_recv_fn on_recv,
                       sfu_on_send_complete_fn on_send_complete, void *user_data);
void sfu_ring_release_packet(sfu_ring_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt);
void sfu_worker_release_packet(sfu_packet_pool_t *pp, sfu_spsc_ring_t *to_dispatcher, sfu_packet_t *pkt);
unsigned sfu_ring_drain_kernel_buffer_returns(sfu_ring_t *r, sfu_spsc_ring_t *from_worker, unsigned max_count);
uint32_t sfu_ring_outstanding_sends(const sfu_ring_t *r);

#endif /* SFU_NET_IO_BACKEND_H */
