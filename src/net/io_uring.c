#include "net/io_uring.h"
#include "net/batch.h"
#include "memory/refcount.h"
#include "sfu/config.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

int sfu_ring_init(sfu_ring_t *r, int fd, uint32_t sq_entries, uint32_t cq_entries,
                   uint32_t buf_count, uint32_t buf_size, int bgid,
                   bool with_recv_bufs) {
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
        SFU_LOG_INFO("io_uring ring initialized (send-only): sq=%u cq=%u",
                     sq_entries, cq_entries);
        return 0;
    }

    if ((buf_count & (buf_count - 1)) != 0) {
        SFU_LOG_ERROR("buf_count must be a power of two (got %u)", buf_count);
        io_uring_queue_exit(&r->ring);
        return -1;
    }

    /* Backing storage for every provided buffer, one contiguous
     * allocation so the whole thing is cache-friendly to walk. */
    r->buf_size  = buf_size;
    r->buf_count = buf_count;
    r->bgid      = bgid;
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

    /* recvmsg with provided buffers: msg_iov must be NULL (payload goes
     * into the provided buffer, not a caller iovec); msg_namelen tells
     * the kernel how much of each buffer to reserve for the source
     * address so io_uring_recvmsg_name()/_payload() can find it later. */
    memset(&r->recv_msg_template, 0, sizeof(r->recv_msg_template));
    r->recv_msg_template.msg_namelen = sizeof(struct sockaddr_storage);

    SFU_LOG_INFO("io_uring ring initialized: sq=%u cq=%u bufs=%u x %uB bgid=%d",
                 sq_entries, cq_entries, buf_count, buf_size, bgid);
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
        SFU_LOG_ERROR("SQ full, cannot arm recv");
        return -1;
    }

    io_uring_prep_recvmsg_multishot(sqe, r->fd, &r->recv_msg_template, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = (uint16_t)r->bgid;
    io_uring_sqe_set_data64(sqe, SFU_CQE_TAG_RECV);

    return 0;
}

int sfu_ring_queue_send_zc(sfu_ring_t *r, sfu_packet_t *pkt,
                            const struct sockaddr *dst, socklen_t dst_len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
    if (!sqe) {
        return -1; /* SQ full: caller should submit() and retry, or drop */
    }

    /* The in-flight send is itself a reference on the buffer -- retain
     * before the kernel can possibly complete (and thus release) it. */
    sfu_packet_retain(pkt, 1);

    io_uring_prep_send_zc(sqe, r->fd, pkt->data, pkt->len, 0, 0);
    io_uring_prep_send_set_addr(sqe, dst, dst_len);

    /* pkt pointers are at least 8-byte aligned (struct alignment), so
     * bit0 is always 0 here -- safe to disambiguate from SFU_CQE_TAG_RECV. */
    io_uring_sqe_set_data(sqe, pkt);

    return 0;
}

int sfu_ring_submit(sfu_ring_t *r) {
    int rc = io_uring_submit(&r->ring);
    if (rc < 0) {
        SFU_LOG_ERROR("io_uring_submit failed: %s", strerror(-rc));
    }
    return rc;
}

void sfu_ring_release_packet(sfu_ring_t *r, sfu_packet_pool_t *pp, sfu_packet_t *pkt) {
    if (!sfu_packet_release(pkt)) {
        return; /* other references still outstanding */
    }

    if (pkt->buf_source == SFU_BUF_SOURCE_KERNEL) {
        int mask = io_uring_buf_ring_mask(r->buf_count);
        void *addr = (uint8_t *)r->buf_ring_mem + (size_t)pkt->kbuf_index * r->buf_size;
        io_uring_buf_ring_add(r->buf_ring, addr, r->buf_size,
                               pkt->kbuf_index, mask, 0);
        io_uring_buf_ring_advance(r->buf_ring, 1);
        sfu_packet_pool_free_meta(pp, pkt);
    } else {
        sfu_packet_pool_free(pp, pkt);
    }
}

void sfu_worker_release_packet(sfu_packet_pool_t *pp, sfu_spsc_ring_t *to_dispatcher,
                                sfu_packet_t *pkt) {
    if (!sfu_packet_release(pkt)) {
        return;
    }

    if (pkt->buf_source == SFU_BUF_SOURCE_KERNEL) {
        /* The metadata slab pool is MPMC-safe (Treiber stack), so this
         * half is fine to do locally from any thread. */
        uint16_t kbuf_index = pkt->kbuf_index;
        sfu_packet_pool_free_meta(pp, pkt);

        /* Only the kernel buffer itself needs to go back through the
         * dispatcher, which is the sole thread allowed to touch its
         * own buf_ring. Pack the index as a tagged pointer-sized value
         * so the SPSC ring's void* slots don't need a separate pool. */
        void *item = (void *)(uintptr_t)((uint64_t)kbuf_index + 1); /* +1: never push NULL */
        if (!sfu_spsc_ring_push(to_dispatcher, item)) {
            SFU_LOG_WARN("release queue to dispatcher full, kernel buffer %u "
                         "temporarily leaked (transient under sustained overload)",
                         kbuf_index);
        }
    } else {
        sfu_packet_pool_free(pp, pkt);
    }
}

unsigned sfu_ring_drain_kernel_buffer_returns(sfu_ring_t *r, sfu_spsc_ring_t *from_worker,
                                               unsigned max_count) {
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

/* Handles one RECV-tagged CQE: validates/unpacks the packed
 * io_uring_recvmsg_out header, wraps the provided buffer into a fresh
 * sfu_packet_t (no copy), and re-arms the multishot request if the
 * kernel terminated it (IORING_CQE_F_MORE unset -- happens on error or
 * when it's been asked to stop, e.g. buffer ring temporarily exhausted). */
static void handle_recv_cqe(sfu_ring_t *r, struct io_uring_cqe *cqe,
                             sfu_packet_pool_t *pp, sfu_on_recv_fn on_recv,
                             void *user_data) {
    if (cqe->res < 0) {
        if (cqe->res != -ENOBUFS) {
            SFU_LOG_WARN("recvmsg cqe error: %s", strerror(-cqe->res));
        }
        goto maybe_rearm;
    }

    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
        SFU_LOG_WARN("recv completion missing buffer id, dropping");
        goto maybe_rearm;
    }

    uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    void *buf = (uint8_t *)r->buf_ring_mem + (size_t)bid * r->buf_size;

    struct io_uring_recvmsg_out *o =
        io_uring_recvmsg_validate(buf, cqe->res, &r->recv_msg_template);
    if (!o) {
        SFU_LOG_WARN("malformed recvmsg_out, dropping (bid=%u)", bid);
        int mask = io_uring_buf_ring_mask(r->buf_count);
        io_uring_buf_ring_add(r->buf_ring, buf, r->buf_size, bid, mask, 0);
        io_uring_buf_ring_advance(r->buf_ring, 1);
        goto maybe_rearm;
    }
    if (o->flags & MSG_TRUNC) {
        SFU_LOG_WARN("datagram truncated (bid=%u), dropping", bid);
        /* still need to recycle the buffer below via normal packet path */
    }

    sfu_packet_t *pkt = sfu_packet_pool_alloc_meta(pp);
    if (!pkt) {
        SFU_LOG_WARN("meta pool exhausted, dropping datagram (backpressure)");
        int mask = io_uring_buf_ring_mask(r->buf_count);
        io_uring_buf_ring_add(r->buf_ring, buf, r->buf_size, bid, mask, 0);
        io_uring_buf_ring_advance(r->buf_ring, 1);
        goto maybe_rearm;
    }

    void *payload = io_uring_recvmsg_payload(o, &r->recv_msg_template);
    uint32_t payload_len = io_uring_recvmsg_payload_length(
        o, cqe->res, &r->recv_msg_template);

    pkt->data       = (uint8_t *)payload;
    pkt->len         = payload_len;
    pkt->cap         = r->buf_size;
    pkt->kbuf_index  = bid;
    pkt->buf_source  = SFU_BUF_SOURCE_KERNEL;

    void *name = io_uring_recvmsg_name(o);
    socklen_t namelen = o->namelen;
    if (namelen > sizeof(pkt->peer_addr)) namelen = sizeof(pkt->peer_addr);
    memcpy(&pkt->peer_addr, name, namelen);
    pkt->peer_addr_len = namelen;

    on_recv(user_data, pkt); /* ownership transferred to callback */

maybe_rearm:
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        if (sfu_ring_arm_recv(r) != 0) {
            SFU_LOG_ERROR("failed to re-arm multishot recv after termination");
        }
    }
}

unsigned sfu_ring_reap(sfu_ring_t *r, unsigned max_count,
                        sfu_packet_pool_t *pp,
                        sfu_spsc_ring_t *release_to_dispatcher,
                        sfu_on_recv_fn on_recv,
                        sfu_on_send_complete_fn on_send_complete,
                        void *user_data) {
    struct io_uring_cqe *cqes[256];
    if (max_count > 256) max_count = 256;

    unsigned n = sfu_batch_peek_cqe(&r->ring, cqes, max_count);
    bool needs_submit = false;

    for (unsigned i = 0; i < n; i++) {
        struct io_uring_cqe *cqe = cqes[i];
        uint64_t data = io_uring_cqe_get_data64(cqe);

        if (data == SFU_CQE_TAG_RECV) {
            handle_recv_cqe(r, cqe, pp, on_recv, user_data);
            /* handle_recv_cqe may have queued a re-arm SQE */
            needs_submit = true;
        } else {
            /* SEND_ZC completion: data is the sfu_packet_t* itself.
             * Only release on the NOTIF (buffer-safe-to-reuse) CQE --
             * the first completion just means "accepted for send". */
            sfu_packet_t *pkt = (sfu_packet_t *)(uintptr_t)data;
            if (cqe->flags & IORING_CQE_F_NOTIF) {
                if (on_send_complete) on_send_complete(user_data, pkt);

                /* r may be a send-only worker ring with no buf_ring of
                 * its own -- if so, release_to_dispatcher routes
                 * kernel-sourced packets back to the dispatcher instead
                 * of touching a buf_ring that doesn't exist here. */
                if (release_to_dispatcher) {
                    sfu_worker_release_packet(pp, release_to_dispatcher, pkt);
                } else {
                    sfu_ring_release_packet(r, pp, pkt);
                }
            } else if (cqe->res < 0) {
                SFU_LOG_WARN("send_zc error: %s", strerror(-cqe->res));
            }
        }
    }

    sfu_batch_advance_cqe(&r->ring, n);

    if (needs_submit) {
        sfu_ring_submit(r);
    }

    return n;
}
