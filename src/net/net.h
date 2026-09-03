#ifndef SFU_NET_IO_H
#define SFU_NET_IO_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "memory/packet_pool.h"
#include "sfu/packet.h"
#include "util/ringbuffer.h"

#define SFU_RECV_CMSG_BUFSIZE 64u

typedef struct sfu_net sfu_net_t;

typedef enum {
  SFU_NET_PRIORITY_NORMAL = 0,
  SFU_NET_PRIORITY_CONTROL = 1,
} sfu_net_send_priority_t;

typedef void (*sfu_net_on_recv_fn)(void *user_data, sfu_packet_t *pkt);
typedef void (*sfu_net_on_send_complete_fn)(void *user_data, sfu_packet_t *pkt);

typedef struct sfu_net_backend_options {
  const char *interface_name;
  const char *queue_spec;
  uint32_t queue_id;
  bool queue_id_set;
  uint16_t media_port;
  uint32_t frame_count;
  uint32_t frame_size;
  const char *xdp_mode;
} sfu_net_backend_options_t;

typedef struct sfu_net_options {
  int fd;
  uint32_t send_entries;
  uint32_t completion_entries;
  uint32_t recv_buffer_count;
  uint32_t recv_buffer_size;
  int buffer_group_id;
  uint32_t queue_slot;
  bool receive;
} sfu_net_options_t;

uint32_t sfu_net_recv_overhead(void);
uint32_t sfu_net_recv_slot_size(uint32_t payload_cap);
uint64_t sfu_net_recv_capacity_bytes(const sfu_net_backend_options_t *backend_options, uint32_t recv_buffer_count, uint32_t payload_cap);

bool sfu_net_backend_is_worker_driven(void);
uint32_t sfu_net_backend_queue_count(void);

int sfu_net_backend_init(int fd, const sfu_net_backend_options_t *options);
void sfu_net_backend_destroy(void);

sfu_net_t *sfu_net_create(const sfu_net_options_t *options);
void sfu_net_destroy(sfu_net_t *net);

int sfu_net_send_ex(sfu_net_t *net, sfu_packet_t *pkt, const struct sockaddr *dst, socklen_t dst_len, sfu_net_send_priority_t priority);
int sfu_net_send(sfu_net_t *net, sfu_packet_t *pkt, const struct sockaddr *dst, socklen_t dst_len);
int sfu_net_recv(sfu_net_t *net);
unsigned sfu_net_poll(sfu_net_t *net, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher,
                      sfu_net_on_recv_fn on_recv, sfu_net_on_send_complete_fn on_send_complete, void *user_data);
int sfu_net_flush(sfu_net_t *net);

unsigned sfu_net_service(sfu_net_t *recv_net, sfu_net_t *send_net, unsigned max_count);
unsigned sfu_net_cancel(sfu_net_t *send_net);
void sfu_net_release_packet(sfu_net_t *net, sfu_packet_pool_t *pp, sfu_packet_t *pkt);
void sfu_net_worker_release_packet(sfu_packet_pool_t *pp, sfu_spsc_ring_t *to_dispatcher, sfu_packet_t *pkt);
bool sfu_net_worker_release_packet_routed(sfu_net_t *net, sfu_packet_pool_t *pp, sfu_packet_t *pkt, uint32_t current_worker,
                                          uint32_t *owner_worker, uintptr_t *token);
unsigned sfu_net_drain_buffer_returns(sfu_net_t *net, sfu_spsc_ring_t *from_worker, unsigned max_count);
bool sfu_net_recycle_rx_token(sfu_net_t *net, uintptr_t token);
uint32_t sfu_net_outstanding_sends(const sfu_net_t *net);

bool sfu_net_test_pending_at(const sfu_net_t *net, sfu_net_send_priority_t priority, uint32_t index, sfu_packet_t **pkt,
                             struct sockaddr_storage *dst, socklen_t *dst_len);
unsigned sfu_net_test_select_priorities(const sfu_net_send_priority_t *priorities, size_t count, sfu_net_send_priority_t *selected,
                                        size_t selected_capacity);

#endif /* SFU_NET_IO_H */
