#include "pipeline/dispatch.h"
#include "transport/stun/stun.h"
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "peer/session.h"
#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "util/log.h"

#include <string.h>

/* Builds a locally-originated packet (POOL-backed, not tied to any
 * kernel buffer) and queues it back to the sender. Used for STUN
 * Binding responses and DTLS handshake flights -- neither is a
 * kernel-sourced RTP packet being forwarded, both are bytes this
 * worker constructed itself. */
static void send_raw(sfu_worker_t *w, const uint8_t *data, size_t len,
                      const struct sockaddr_storage *dst, socklen_t dst_len) {
    if (len == 0) return;

    sfu_packet_t *out = sfu_packet_pool_alloc(w->pp);
    if (!out) {
        SFU_LOG_WARN("packet pool exhausted, dropping handshake response");
        return;
    }
    if (len > out->cap) {
        SFU_LOG_WARN("handshake response too large (%zu > %u), dropping", len, out->cap);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, out);
        return;
    }

    memcpy(out->data, data, len);
    out->len = (uint32_t)len;

    if (sfu_ring_queue_send_zc(&w->send_ring, out, (const struct sockaddr *)dst, dst_len) != 0) {
        SFU_LOG_WARN("worker %u: send SQ full, dropping handshake response", w->worker_index);
    }

    /* Drop our own initial reference; the in-flight send (if it was
     * queued successfully) holds its own via sfu_ring_queue_send_zc's
     * internal retain. Submit immediately rather than waiting for the
     * next batch tick -- handshake latency matters. */
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, out);
    sfu_ring_submit(&w->send_ring);
}

static void handle_stun(sfu_worker_t *w, sfu_packet_t *pkt) {
    uint8_t response[512];
    size_t response_len = sfu_stun_handle_binding_request(
        pkt->data, pkt->len, w->ice_creds, &pkt->peer_addr, pkt->peer_addr_len,
        response, sizeof(response));

    if (response_len > 0) {
        send_raw(w, response, response_len, &pkt->peer_addr, pkt->peer_addr_len);
    }
    /* Invalid/unauthenticated requests are silently dropped per
     * sfu_stun_handle_binding_request's contract -- no reply either way. */
}

static void handle_dtls(sfu_worker_t *w, sfu_packet_t *pkt) {
    sfu_peer_session_t *session = sfu_session_table_get_or_create(
        w->sessions, &pkt->peer_addr, pkt->peer_addr_len);
    if (!session) {
        return; /* table full; drop, same as any other admission-control rejection */
    }

    sfu_dtls_feed_status_t status = sfu_dtls_conn_feed(&session->dtls, pkt->data, pkt->len);

    uint8_t out[4096];
    size_t out_len = sfu_dtls_conn_drain_output(&session->dtls, out, sizeof(out));
    if (out_len > 0) {
        send_raw(w, out, out_len, &pkt->peer_addr, pkt->peer_addr_len);
    }

    switch (status) {
        case SFU_DTLS_FEED_ESTABLISHED:
            if (session->state != SFU_SESSION_ESTABLISHED) {
                if (sfu_srtp_ctx_init_from_dtls(&session->srtp,
                                                 session->dtls.srtp_keying_material) != 0) {
                    SFU_LOG_ERROR("worker %u: failed to derive SRTP keys after DTLS handshake",
                                 w->worker_index);
                    session->state = SFU_SESSION_FAILED;
                    break;
                }
                session->state = SFU_SESSION_ESTABLISHED;
                SFU_LOG_INFO("worker %u: DTLS established, SRTP sessions ready", w->worker_index);
            }
            break;
        case SFU_DTLS_FEED_IN_PROGRESS:
            session->state = SFU_SESSION_DTLS_HANDSHAKING;
            break;
        case SFU_DTLS_FEED_ERROR:
            session->state = SFU_SESSION_FAILED;
            SFU_LOG_WARN("worker %u: DTLS handshake failed for a peer", w->worker_index);
            break;
    }
}

void sfu_dispatch_packet(sfu_worker_t *w, sfu_packet_t *pkt) {
    if (sfu_stun_is_stun_packet(pkt->data, pkt->len)) {
        handle_stun(w, pkt);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
        return;
    }

    if (sfu_dtls_is_dtls_packet(pkt->data, pkt->len)) {
        handle_dtls(w, pkt);
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
        return;
    }

    /* Anything else (RTP/RTCP range, RFC 7983: first byte 128-191) is
     * SRTP-protected media. sfu_room_forward_packet now does the real
     * work: decrypt once with the sender's session, re-encrypt once per
     * subscriber with that subscriber's own key, drop entirely if the
     * sender has no established session (can't decrypt without keys). */
    sfu_room_forward_packet(w, pkt);
}
