#include "net/net.h"
#include "memory/refcount.h"
#include "memory/worker_packet_arena.h"
#include "net/batch.h"
#include "sfu/config.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/metrics.h"

#include <arpa/inet.h>
#include <liburing.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define SFU_CQE_PKT_PTR_MASK 0x0000ffffffffffffULL
#define SFU_CQE_PKT_GEN_SHIFT 48
#define SFU_CQE_TAG_RECV 0x1ULL

struct sfu_net {
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
};

static uint64_t sfu_cqe_pack_packet(sfu_packet_t *pkt) {
  uint32_t gen = sfu_packet_generation(pkt) & 0xffffu;
  return ((uint64_t)gen << SFU_CQE_PKT_GEN_SHIFT) | ((uintptr_t)pkt & SFU_CQE_PKT_PTR_MASK);
}

static sfu_packet_t *sfu_cqe_unpack_packet(uint64_t data) {
  sfu_packet_t *pkt = (sfu_packet_t *)(uintptr_t)(data & SFU_CQE_PKT_PTR_MASK);
  uint32_t gen = (uint32_t)(data >> SFU_CQE_PKT_GEN_SHIFT);
  if (!pkt || (sfu_packet_generation(pkt) & 0xffffu) != gen) {
    return NULL;
  }
  return pkt;
}

uint32_t sfu_net_recv_overhead(void) {
  return (uint32_t)sizeof(struct io_uring_recvmsg_out) + (uint32_t)sizeof(struct sockaddr_storage) + SFU_RECV_CMSG_BUFSIZE;
}

uint32_t sfu_net_recv_slot_size(uint32_t payload_cap) { return payload_cap + sfu_net_recv_overhead(); }

uint64_t sfu_net_recv_capacity_bytes(const sfu_net_backend_options_t *backend_options, uint32_t recv_buffer_count, uint32_t payload_cap) {
  (void)backend_options;
  return (uint64_t)recv_buffer_count * sfu_net_recv_slot_size(payload_cap);
}

static void format_ring_peer_addr(const struct sockaddr *addr, char *out_ip, uint16_t *out_port) {
  strcpy(out_ip, "unknown");
  *out_port = 0;
  if (!addr) {
    return;
  }
  if (addr->sa_family == AF_INET) {
    struct sockaddr_in *s4 = (struct sockaddr_in *)addr;
    inet_ntop(AF_INET, &s4->sin_addr, out_ip, 64);
    *out_port = ntohs(s4->sin_port);
  } else if (addr->sa_family == AF_INET6) {
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)addr;
    inet_ntop(AF_INET6, &s6->sin6_addr, out_ip, 64);
    *out_port = ntohs(s6->sin6_port);
  }
}

sfu_net_t *sfu_net_create(const sfu_net_options_t *options) {
  if (!options) {
    return NULL;
  }
  sfu_net_t *r = SFU_CALLOC(1, sizeof(*r));
  if (!r) {
    return NULL;
  }
  int fd = options->fd;
  uint32_t sq_entries = options->send_entries;
  uint32_t cq_entries = options->completion_entries;
  uint32_t buf_count = options->recv_buffer_count;
  uint32_t buf_size = options->recv_buffer_size;
  int bgid = options->buffer_group_id;
  bool with_recv_bufs = options->receive;
  r->fd = fd;

  struct io_uring_params params;
  memset(&params, 0, sizeof(params));
  params.flags = IORING_SETUP_CQSIZE;
  params.cq_entries = cq_entries;

  int rc = io_uring_queue_init_params(sq_entries, &r->ring, &params);
  if (rc < 0) {
    SFU_LOG_ERROR("io_uring_queue_init_params failed: %s", strerror(-rc));
    SFU_FREE(r);
    return NULL;
  }

  if (!with_recv_bufs) {
    SFU_LOG_INFO("io_uring ring initialized (send-only): sq=%u cq=%u on fd=%d", sq_entries, cq_entries, fd);
    return r;
  }

  if ((buf_count & (buf_count - 1)) != 0) {
    SFU_LOG_ERROR("buf_count must be a power of two (got %u)", buf_count);
    io_uring_queue_exit(&r->ring);
    SFU_FREE(r);
    return NULL;
  }

  r->payload_cap = buf_size;
  r->buf_size = sfu_net_recv_slot_size(buf_size);
  r->buf_count = buf_count;
  r->bgid = bgid;
  r->buf_ring_mem = SFU_ALIGNED_ALLOC(SFU_CACHELINE_SIZE, (size_t)buf_count * r->buf_size);
  if (!r->buf_ring_mem) {
    SFU_LOG_ERROR("failed to allocate provided-buffer backing store");
    io_uring_queue_exit(&r->ring);
    SFU_FREE(r);
    return NULL;
  }

  int setup_ret = 0;
  r->buf_ring = io_uring_setup_buf_ring(&r->ring, buf_count, bgid, 0, &setup_ret);
  if (!r->buf_ring) {
    SFU_LOG_ERROR("io_uring_setup_buf_ring failed: %s", strerror(-setup_ret));
    SFU_FREE(r->buf_ring_mem);
    io_uring_queue_exit(&r->ring);
    SFU_FREE(r);
    return NULL;
  }

  int mask = io_uring_buf_ring_mask(buf_count);
  for (uint32_t i = 0; i < buf_count; i++) {
    void *addr = (uint8_t *)r->buf_ring_mem + (size_t)i * r->buf_size;
    io_uring_buf_ring_add(r->buf_ring, addr, r->buf_size, (unsigned short)i, mask, (int)i);
  }
  io_uring_buf_ring_advance(r->buf_ring, (int)buf_count);

  memset(&r->recv_msg_template, 0, sizeof(r->recv_msg_template));
  r->recv_msg_template.msg_namelen = sizeof(struct sockaddr_storage);
  r->recv_msg_template.msg_control = r->recv_cmsg_buf;
  r->recv_msg_template.msg_controllen = sizeof(r->recv_cmsg_buf);

  SFU_LOG_INFO("io_uring ring initialized: sq=%u cq=%u bufs=%u x %uB slot (payload=%u + overhead=%u) bgid=%d on fd=%d", sq_entries, cq_entries, buf_count,
               r->buf_size, r->payload_cap, sfu_net_recv_overhead(), bgid, fd);
  return r;
}

void sfu_net_destroy(sfu_net_t *r) {
  if (!r) {
    return;
  }
  if (r->buf_ring) {
    io_uring_free_buf_ring(&r->ring, r->buf_ring, r->buf_count, r->bgid);
    r->buf_ring = NULL;
  }
  SFU_FREE(r->buf_ring_mem);
  r->buf_ring_mem = NULL;
  io_uring_queue_exit(&r->ring);
  SFU_FREE(r);
}

int sfu_net_recv(sfu_net_t *r) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
  if (!sqe) {
    SFU_LOG_ERROR("SQ full, cannot arm recv on fd=%d", r->fd);
    return -1;
  }

  io_uring_prep_recvmsg_multishot(sqe, r->fd, &r->recv_msg_template, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = (uint16_t)r->bgid;
  io_uring_sqe_set_data64(sqe, SFU_CQE_TAG_RECV);

  return 0;
}

int sfu_net_send(sfu_net_t *r, sfu_packet_t *pkt, const struct sockaddr *dst, socklen_t dst_len) {
  if (!r || !pkt || !dst || dst_len == 0 || (size_t)dst_len > sizeof(pkt->peer_addr)) {
    return -1;
  }

  struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
  if (!sqe) {
    return -1;
  }

  memcpy(&pkt->peer_addr, dst, dst_len);
  pkt->peer_addr_len = dst_len;
  sfu_packet_retain(pkt, 1);
  io_uring_prep_send_zc(sqe, r->fd, pkt->data, pkt->len, 0, 0);
  io_uring_prep_send_set_addr(sqe, (const struct sockaddr *)&pkt->peer_addr, pkt->peer_addr_len);
  io_uring_sqe_set_data64(sqe, sfu_cqe_pack_packet(pkt));
  r->outstanding_sends++;

  return 0;
}

int sfu_net_flush(sfu_net_t *r) {
  int rc = io_uring_submit(&r->ring);
  if (rc < 0) {
    SFU_LOG_ERROR("io_uring_submit failed on fd=%d: %s", r->fd, strerror(-rc));
  }
  return rc;
}

void sfu_net_release_packet(sfu_net_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt) {
  if (!sfu_packet_release(pkt)) {
    return;
  }

  if (pkt->buf_source == SFU_BUF_SOURCE_KERNEL) {
    int mask = io_uring_buf_ring_mask(r->buf_count);
    void *addr = (uint8_t *)r->buf_ring_mem + (size_t)pkt->kbuf_index * r->buf_size;
    io_uring_buf_ring_add(r->buf_ring, addr, r->buf_size, pkt->kbuf_index, mask, 0);
    io_uring_buf_ring_advance(r->buf_ring, 1);
    sfu_packet_pool_free_meta(pp, pkt);
  } else if (pkt->buf_source == SFU_BUF_SOURCE_WORKER_ARENA) {
    sfu_worker_packet_arena_free(pkt);
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

void sfu_net_worker_release_packet(sfu_packet_pool_t *pp, sfu_spsc_ring_t *to_dispatcher, sfu_packet_t *pkt) {
  if (!sfu_packet_release(pkt)) {
    return;
  }

  if (pkt->buf_source == SFU_BUF_SOURCE_KERNEL) {
    uint16_t kbuf_index = pkt->kbuf_index;
    sfu_packet_pool_free_meta(pp, pkt);

    void *item = (void *)(uintptr_t)((uint64_t)kbuf_index + 1);
    while (!sfu_spsc_ring_push(to_dispatcher, item)) {
      sched_yield();
    }
  } else if (pkt->buf_source == SFU_BUF_SOURCE_WORKER_ARENA) {
    sfu_worker_packet_arena_free(pkt);
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

unsigned sfu_net_drain_buffer_returns(sfu_net_t *r, sfu_spsc_ring_t *from_worker, unsigned max_count) {
  unsigned n = 0;
  int mask = io_uring_buf_ring_mask(r->buf_count);
  void *item;

  while (n < max_count && sfu_spsc_ring_pop(from_worker, &item)) {
    uint16_t kbuf_index = (uint16_t)(((uint64_t)(uintptr_t)item) - 1);
    void *addr = (uint8_t *)r->buf_ring_mem + (size_t)kbuf_index * r->buf_size;
    io_uring_buf_ring_add(r->buf_ring, addr, r->buf_size, kbuf_index, mask, (int)n);
    n++;
  }

  if (n > 0) {
    io_uring_buf_ring_advance(r->buf_ring, (int)n);
  }
  return n;
}

static void handle_recv_cqe(sfu_net_t *r, struct io_uring_cqe *cqe, sfu_packet_pool_t *pp, sfu_net_on_recv_fn on_recv, void *user_data) {
  if (cqe->res < 0) {
    if (cqe->res == -ENOBUFS) {
      SFU_LOG_WARN("CRITICAL: io_uring kernel drop on fd=%d: ENOBUFS (Provided buffer ring is entirely full!)", r->fd);
    } else {
      SFU_LOG_WARN("recvmsg cqe kernel error on fd=%d: %s", r->fd, strerror(-cqe->res));
    }
    goto maybe_rearm;
  }

  if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
    SFU_LOG_WARN("recv completion missing buffer id on fd=%d, dropping", r->fd);
    goto maybe_rearm;
  }

  uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
  void *buf = (uint8_t *)r->buf_ring_mem + (size_t)bid * r->buf_size;

  struct io_uring_recvmsg_out *o = io_uring_recvmsg_validate(buf, cqe->res, &r->recv_msg_template);
  if (!o) {
    SFU_LOG_WARN("malformed recvmsg_out on fd=%d, dropping (bid=%u)", r->fd, bid);
    int mask = io_uring_buf_ring_mask(r->buf_count);
    io_uring_buf_ring_add(r->buf_ring, buf, r->buf_size, bid, mask, 0);
    io_uring_buf_ring_advance(r->buf_ring, 1);
    goto maybe_rearm;
  }

  void *name = io_uring_recvmsg_name(o);
  char peer_ip[64];
  uint16_t peer_port;
  format_ring_peer_addr((const struct sockaddr *)name, peer_ip, &peer_port);

  uint64_t kernel_recv_ts_ns = 0;
  for (struct cmsghdr *cmsg = io_uring_recvmsg_cmsg_firsthdr(o, &r->recv_msg_template); cmsg;
       cmsg = io_uring_recvmsg_cmsg_nexthdr(o, &r->recv_msg_template, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPNS) {
      const struct timespec *ts = (const struct timespec *)CMSG_DATA(cmsg);
      kernel_recv_ts_ns = (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
      break;
    }
  }

  if (o->flags & MSG_TRUNC) {
    SFU_LOG_WARN("datagram truncated from %s:%u (bid=%u), dropping", peer_ip, peer_port, bid);
    int mask = io_uring_buf_ring_mask(r->buf_count);
    io_uring_buf_ring_add(r->buf_ring, buf, r->buf_size, bid, mask, 0);
    io_uring_buf_ring_advance(r->buf_ring, 1);
    goto maybe_rearm;
  }

  sfu_packet_t *pkt = sfu_packet_pool_alloc_meta(pp);
  if (!pkt) {
    SFU_LOG_WARN("meta pool exhausted on fd=%d, dropping datagram from %s:%u", r->fd, peer_ip, peer_port);
    int mask = io_uring_buf_ring_mask(r->buf_count);
    io_uring_buf_ring_add(r->buf_ring, buf, r->buf_size, bid, mask, 0);
    io_uring_buf_ring_advance(r->buf_ring, 1);
    goto maybe_rearm;
  }

  void *payload = io_uring_recvmsg_payload(o, &r->recv_msg_template);
  uint32_t payload_len = io_uring_recvmsg_payload_length(o, cqe->res, &r->recv_msg_template);
  if (!payload || payload_len == 0) {
    SFU_LOG_WARN("recv completion with empty payload on fd=%d from %s:%u (bid=%u len=%u), dropping", r->fd, peer_ip, peer_port, bid, payload_len);
    sfu_packet_pool_free_meta(pp, pkt);
    int mask = io_uring_buf_ring_mask(r->buf_count);
    io_uring_buf_ring_add(r->buf_ring, buf, r->buf_size, bid, mask, 0);
    io_uring_buf_ring_advance(r->buf_ring, 1);
    goto maybe_rearm;
  }

  pkt->data = (uint8_t *)payload;
  pkt->len = payload_len;
  pkt->cap = r->payload_cap;
  pkt->kbuf_index = bid;
  pkt->buf_source = SFU_BUF_SOURCE_KERNEL;

  socklen_t namelen = o->namelen;
  if (namelen > sizeof(pkt->peer_addr)) {
    namelen = sizeof(pkt->peer_addr);
  }
  memcpy(&pkt->peer_addr, name, namelen);
  pkt->peer_addr_len = namelen;
  pkt->recv_ts_ns = kernel_recv_ts_ns;

  on_recv(user_data, pkt);

maybe_rearm:
  if (!(cqe->flags & IORING_CQE_F_MORE)) {
    if (sfu_net_recv(r) != 0) {
      SFU_LOG_ERROR("failed to re-arm multishot recv after termination on fd=%d", r->fd);
    }
  }
}

unsigned sfu_net_poll(sfu_net_t *r, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher, sfu_net_on_recv_fn on_recv,
                       sfu_net_on_send_complete_fn on_send_complete, void *user_data) {
  struct io_uring_cqe *cqes[256];
  if (max_count > 256) {
    max_count = 256;
  }

  unsigned n = sfu_batch_peek_cqe(&r->ring, cqes, max_count);
  bool needs_submit = false;

  for (unsigned i = 0; i < n; i++) {
    struct io_uring_cqe *cqe = cqes[i];
    uint64_t data = io_uring_cqe_get_data64(cqe);

    if (data == SFU_CQE_TAG_RECV) {
      handle_recv_cqe(r, cqe, pp, on_recv, user_data);
      needs_submit = true;
    } else {
      sfu_packet_t *pkt = sfu_cqe_unpack_packet(data);
      if (!pkt) {
        sfu_metric_inc("send_cqe_stale_packet");
        SFU_LOG_WARN("send_zc completion for recycled packet on fd=%d flags=0x%x res=%d", r->fd, cqe->flags, cqe->res);
        continue;
      }
      if (cqe->flags & IORING_CQE_F_NOTIF) {
        if (r->outstanding_sends > 0) {
          r->outstanding_sends--;
        }
        if (on_send_complete) {
          on_send_complete(user_data, pkt);
        }

        if (release_to_dispatcher) {
          sfu_net_worker_release_packet(pp, release_to_dispatcher, pkt);
        } else {
          sfu_net_release_packet(r, pp, pkt);
        }
      } else {
        if (cqe->res < 0) {
          SFU_LOG_WARN("send_zc error on fd=%d: %s", r->fd, strerror(-cqe->res));
        }
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
          if (r->outstanding_sends > 0) {
            r->outstanding_sends--;
          }
          if (release_to_dispatcher) {
            sfu_net_worker_release_packet(pp, release_to_dispatcher, pkt);
          } else {
            sfu_net_release_packet(r, pp, pkt);
          }
        }
      }
    }
  }

  sfu_batch_advance_cqe(&r->ring, n);

  if (needs_submit) {
    sfu_net_flush(r);
  }

  return n;
}

int sfu_net_backend_init(int fd, const sfu_net_backend_options_t *options) {
  (void)fd;
  (void)options;
  return 0;
}

void sfu_net_backend_destroy(void) {}

unsigned sfu_net_service(sfu_net_t *recv_net, sfu_net_t *send_net, unsigned max_count) {
  (void)recv_net;
  (void)send_net;
  (void)max_count;
  return 0;
}

unsigned sfu_net_cancel(sfu_net_t *send_net) {
  (void)send_net;
  return 0;
}

uint32_t sfu_net_outstanding_sends(const sfu_net_t *r) { return r ? atomic_load_explicit(&r->outstanding_sends, memory_order_relaxed) : 0; }
