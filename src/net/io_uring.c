#include "net/io_uring.h"
#include "memory/refcount.h"
#include "net/batch.h"
#include "sfu/config.h"
#include "util/alloc.h"
#include "util/log.h"

#include <arpa/inet.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* Helper to safely format address data at the low-level ring layer */
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

int sfu_ring_init(sfu_ring_t *r, int fd, uint32_t sq_entries, uint32_t cq_entries, uint32_t buf_count, uint32_t buf_size, int bgid, bool with_recv_bufs) {
  memset(r, 0, sizeof(*r));
  r->fd = fd;

  struct io_uring_params params;
  memset(&params, 0, sizeof(params));
  params.flags = IORING_SETUP_CQSIZE;
  params.cq_entries = cq_entries;

  int rc = io_uring_queue_init_params(sq_entries, &r->ring, &params);
  if (rc < 0) {
    SFU_LOG_ERROR("io_uring_queue_init_params failed: %s", strerror(-rc));
    return -1;
  }

  if (!with_recv_bufs) {
    SFU_LOG_INFO("io_uring ring initialized (send-only): sq=%u cq=%u on fd=%d", sq_entries, cq_entries, fd);
    return 0;
  }

  if ((buf_count & (buf_count - 1)) != 0) {
    SFU_LOG_ERROR("buf_count must be a power of two (got %u)", buf_count);
    io_uring_queue_exit(&r->ring);
    return -1;
  }

  r->buf_size = buf_size;
  r->buf_count = buf_count;
  r->bgid = bgid;
  r->buf_ring_mem = SFU_ALIGNED_ALLOC(SFU_CACHELINE_SIZE, (size_t)buf_count * buf_size);
  if (!r->buf_ring_mem) {
    SFU_LOG_ERROR("failed to allocate provided-buffer backing store");
    io_uring_queue_exit(&r->ring);
    return -1;
  }

  int setup_ret = 0;
  r->buf_ring = io_uring_setup_buf_ring(&r->ring, buf_count, bgid, 0, &setup_ret);
  if (!r->buf_ring) {
    SFU_LOG_ERROR("io_uring_setup_buf_ring failed: %s", strerror(-setup_ret));
    SFU_FREE(r->buf_ring_mem);
    io_uring_queue_exit(&r->ring);
    return -1;
  }

  int mask = io_uring_buf_ring_mask(buf_count);
  for (uint32_t i = 0; i < buf_count; i++) {
    void *addr = (uint8_t *)r->buf_ring_mem + (size_t)i * buf_size;
    io_uring_buf_ring_add(r->buf_ring, addr, buf_size, (unsigned short)i, mask, (int)i);
  }
  io_uring_buf_ring_advance(r->buf_ring, (int)buf_count);

  memset(&r->recv_msg_template, 0, sizeof(r->recv_msg_template));
  r->recv_msg_template.msg_namelen = sizeof(struct sockaddr_storage);

  SFU_LOG_INFO("io_uring ring initialized: sq=%u cq=%u bufs=%u x %uB bgid=%d on fd=%d", sq_entries, cq_entries, buf_count, buf_size, bgid, fd);
  return 0;
}

void sfu_ring_destroy(sfu_ring_t *r) {
  if (r->buf_ring) {
    io_uring_free_buf_ring(&r->ring, r->buf_ring, r->buf_count, r->bgid);
    r->buf_ring = NULL;
  }
  SFU_FREE(r->buf_ring_mem);
  r->buf_ring_mem = NULL;
  io_uring_queue_exit(&r->ring);
}

int sfu_ring_arm_recv(sfu_ring_t *r) {
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

int sfu_ring_queue_send_zc(sfu_ring_t *r, sfu_packet_t *pkt, const struct sockaddr *dst, socklen_t dst_len) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
  if (!sqe) {
    return -1;
  }

  sfu_packet_retain(pkt, 1);
  io_uring_prep_send_zc(sqe, r->fd, pkt->data, pkt->len, 0, 0);
  io_uring_prep_send_set_addr(sqe, dst, dst_len);
  io_uring_sqe_set_data(sqe, pkt);

  return 0;
}

int sfu_ring_submit(sfu_ring_t *r) {
  int rc = io_uring_submit(&r->ring);
  if (rc < 0) {
    SFU_LOG_ERROR("io_uring_submit failed on fd=%d: %s", r->fd, strerror(-rc));
  }
  return rc;
}

void sfu_ring_release_packet(sfu_ring_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt) {
  if (!sfu_packet_release(pkt)) {
    return;
  }

  if (pkt->buf_source == SFU_BUF_SOURCE_KERNEL) {
    int mask = io_uring_buf_ring_mask(r->buf_count);
    void *addr = (uint8_t *)r->buf_ring_mem + (size_t)pkt->kbuf_index * r->buf_size;
    io_uring_buf_ring_add(r->buf_ring, addr, r->buf_size, pkt->kbuf_index, mask, 0);
    io_uring_buf_ring_advance(r->buf_ring, 1);
    sfu_packet_pool_free_meta(pp, pkt);
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

void sfu_worker_release_packet(sfu_packet_pool_t *pp, sfu_spsc_ring_t *to_dispatcher, sfu_packet_t *pkt) {
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
  } else {
    sfu_packet_pool_free(pp, pkt);
  }
}

unsigned sfu_ring_drain_kernel_buffer_returns(sfu_ring_t *r, sfu_spsc_ring_t *from_worker, unsigned max_count) {
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

static void handle_recv_cqe(sfu_ring_t *r, struct io_uring_cqe *cqe, sfu_packet_pool_t *pp, sfu_on_recv_fn on_recv, void *user_data) {
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

  /* Log raw packet arrival immediately after successful layout validation */
  void *name = io_uring_recvmsg_name(o);
  char peer_ip[64];
  uint16_t peer_port;
  format_ring_peer_addr((const struct sockaddr *)name, peer_ip, &peer_port);

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

  pkt->data = (uint8_t *)payload;
  pkt->len = payload_len;
  pkt->cap = r->buf_size;
  pkt->kbuf_index = bid;
  pkt->buf_source = SFU_BUF_SOURCE_KERNEL;

  socklen_t namelen = o->namelen;
  if (namelen > sizeof(pkt->peer_addr)) {
    namelen = sizeof(pkt->peer_addr);
  }
  memcpy(&pkt->peer_addr, name, namelen);
  pkt->peer_addr_len = namelen;

  on_recv(user_data, pkt);

maybe_rearm:
  if (!(cqe->flags & IORING_CQE_F_MORE)) {
    if (sfu_ring_arm_recv(r) != 0) {
      SFU_LOG_ERROR("failed to re-arm multishot recv after termination on fd=%d", r->fd);
    }
  }
}

unsigned sfu_ring_reap(sfu_ring_t *r, unsigned max_count, sfu_packet_pool_t *pp, sfu_spsc_ring_t *release_to_dispatcher, sfu_on_recv_fn on_recv,
                       sfu_on_send_complete_fn on_send_complete, void *user_data) {
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
      sfu_packet_t *pkt = (sfu_packet_t *)(uintptr_t)data;
      if (cqe->flags & IORING_CQE_F_NOTIF) {
        if (on_send_complete) {
          on_send_complete(user_data, pkt);
        }

        if (release_to_dispatcher) {
          sfu_worker_release_packet(pp, release_to_dispatcher, pkt);
        } else {
          sfu_ring_release_packet(r, pp, pkt);
        }
      } else if (cqe->res < 0) {
        SFU_LOG_WARN("send_zc error on fd=%d: %s", r->fd, strerror(-cqe->res));
      }
    }
  }

  sfu_batch_advance_cqe(&r->ring, n);

  if (needs_submit) {
    sfu_ring_submit(r);
  }

  return n;
}
