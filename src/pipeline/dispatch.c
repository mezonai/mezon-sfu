#include "pipeline/dispatch.h"
#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "transport/stun/stun.h"
#include "util/log.h"

#include <arpa/inet.h>
#include <string.h>

/* Helper to convert socket storage to string for clean diagnostic logging */
static void format_peer_endpoint(const struct sockaddr_storage *addr, char *out_ip, uint16_t *out_port) {
  strcpy(out_ip, "unknown");
  *out_port = 0;
  if (addr->ss_family == AF_INET) {
    struct sockaddr_in *s4 = (struct sockaddr_in *)addr;
    inet_ntop(AF_INET, &s4->sin_addr, out_ip, 64);
    *out_port = ntohs(s4->sin_port);
  } else if (addr->ss_family == AF_INET6) {
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)addr;
    inet_ntop(AF_INET6, &s6->sin6_addr, out_ip, 64);
    *out_port = ntohs(s6->sin6_port);
  }
}

static void send_raw(sfu_worker_t *w, const uint8_t *data, size_t len, const struct sockaddr_storage *dst, socklen_t dst_len) {
  if (len == 0) {
    return;
  }

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

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, out);
  sfu_ring_submit(&w->send_ring);
}

static void handle_stun(sfu_worker_t *w, sfu_packet_t *pkt) {
  char ip[64];
  uint16_t port;
  format_peer_endpoint(&pkt->peer_addr, ip, &port);

  uint8_t response[512];
  size_t response_len = sfu_stun_handle_binding_request(pkt->data, pkt->len, w->ice_creds, &pkt->peer_addr, pkt->peer_addr_len, response, sizeof(response));

  if (response_len > 0) {
    SFU_LOG_INFO("worker %u: Responding to STUN Binding Request from %s:%u", w->worker_index, ip, port);
    send_raw(w, response, response_len, &pkt->peer_addr, pkt->peer_addr_len);
  } else {
    SFU_LOG_WARN("worker %u: STUN Request from %s:%u FAILED verification (invalid credentials/bad integrity)", w->worker_index, ip, port);
  }
}

static void handle_dtls(sfu_worker_t *w, sfu_packet_t *pkt) {
  char ip[64];
  uint16_t port;
  format_peer_endpoint(&pkt->peer_addr, ip, &port);

  sfu_peer_session_t *session = sfu_session_table_get_or_create(w->sessions, &pkt->peer_addr, pkt->peer_addr_len);
  if (!session) {
    SFU_LOG_ERROR("worker %u: CRITICAL! Session table full or rejected registration for %s:%u", w->worker_index, ip, port);
    return;
  }

  SFU_LOG_INFO("worker %u: Feeding %u bytes of DTLS data from %s:%u (current state: %d)", w->worker_index, pkt->len, ip, port, session->state);

  sfu_dtls_feed_status_t status = sfu_dtls_conn_feed(&session->dtls, pkt->data, pkt->len);

  uint8_t out[4096];
  size_t out_len = sfu_dtls_conn_drain_output(&session->dtls, out, sizeof(out));
  if (out_len > 0) {
    send_raw(w, out, out_len, &pkt->peer_addr, pkt->peer_addr_len);
  }

  switch (status) {
    case SFU_DTLS_FEED_ESTABLISHED:
      if (session->state != SFU_SESSION_ESTABLISHED) {
        if (sfu_srtp_ctx_init_from_dtls(&session->srtp, session->dtls.srtp_keying_material, session->dtls.srtp_profile_id, true) != 0) {
          SFU_LOG_ERROR("worker %u: failed to derive SRTP keys after DTLS handshake for %s:%u", w->worker_index, ip, port);
          session->state = SFU_SESSION_FAILED;
          break;
        }
        session->state = SFU_SESSION_ESTABLISHED;
        SFU_LOG_INFO("worker %u: DTLS established, SRTP sessions ready for %s:%u", w->worker_index, ip, port);
      }
      break;
    case SFU_DTLS_FEED_IN_PROGRESS:
      session->state = SFU_SESSION_DTLS_HANDSHAKING;
      SFU_LOG_INFO("worker %u: DTLS handshake in progress for %s:%u", w->worker_index, ip, port);
      break;
    case SFU_DTLS_FEED_ERROR:
      session->state = SFU_SESSION_FAILED;
      SFU_LOG_WARN("worker %u: DTLS handshake failed for peer %s:%u", w->worker_index, ip, port);
      break;
  }
}

void sfu_dispatch_packet(sfu_worker_t *w, sfu_packet_t *pkt) {
  char ip[64];
  uint16_t port;
  format_peer_endpoint(&pkt->peer_addr, ip, &port);

  // High volume raw entry log
  SFU_LOG_DEBUG("worker %u: Raw UDP dispatch received %u bytes from %s:%u", w->worker_index, pkt->len, ip, port);

  if (sfu_stun_is_stun_packet(pkt->data, pkt->len)) {
    SFU_LOG_INFO("worker %u: Identified STUN packet from %s:%u", w->worker_index, ip, port);
    handle_stun(w, pkt);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  if (sfu_dtls_is_dtls_packet(pkt->data, pkt->len)) {
    SFU_LOG_INFO("worker %u: Identified DTLS packet from %s:%u", w->worker_index, ip, port);
    handle_dtls(w, pkt);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  /* Media traffic */
  sfu_room_forward_packet(w, pkt);
}
