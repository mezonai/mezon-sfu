#include "pipeline/dispatch.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <string.h>
#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "pipeline/ingress.h"
#include "protocol/signaling/signaling.h"
#include "room/room_media_graph.h"
#include "runtime/routing_context.h"
#include "runtime/scheduler.h"
#include "runtime/timer.h"
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "transport/stun/stun.h"
#include "util/log.h"
#include "util/metrics.h"

extern bool sfu_lookup_ufrag_room(const char *client_ufrag, sfu_room_t **out_room);

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

  SFU_LOG_DEBUG("SEND_ZC worker=%u pkt=%p len=%lu", w->worker_index, data, len);

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

  char client_ufrag[32];
  bool have_ufrag = sfu_stun_extract_client_ufrag(pkt->data, pkt->len, w->ice_creds->ufrag, client_ufrag, sizeof(client_ufrag));

  if (!have_ufrag) {
    SFU_LOG_WARN(
        "worker %u: STUN request from %s:%u has no parseable client ufrag "
        "(malformed USERNAME attribute, or doesn't match our local ufrag prefix) "
        "-- cross-worker ownership tracking and room binding will NOT work for "
        "this peer until a valid one is seen",
        w->worker_index, ip, port);
  }

  uint8_t response[512] = {0};
  size_t response_len = sfu_stun_handle_binding_request(pkt->data, pkt->len, w->ice_creds, &pkt->peer_addr, pkt->peer_addr_len, response, sizeof(response));

  if (response_len == 0) {
    SFU_LOG_WARN("worker %u: STUN Request from %s:%u FAILED verification (invalid credentials/bad integrity)", w->worker_index, ip, port);
    return;
  }

  if (!have_ufrag) {
    return;
  }

  bool nominated = sfu_stun_has_use_candidate(pkt->data, pkt->len);
  sfu_routing_snapshot_t route;
  if (!sfu_routing_table_lookup_route(w->routing_table, client_ufrag, w->worker_index, &route) || !route.room || route.fd < 0) {
    SFU_LOG_DEBUG(
        "worker %u: no signaling route yet for ufrag=%s (from %s:%u); "
        "withholding STUN response until answer registration",
        w->worker_index, client_ufrag, ip, port);
    return;
  }

  SFU_LOG_DEBUG("worker %u: Responding to STUN Binding Request from %s:%u", w->worker_index, ip, port);
  send_raw(w, response, response_len, &pkt->peer_addr, pkt->peer_addr_len);

  sfu_peer_session_t *session = NULL;
  sfu_session_rebind_result_t rebind_result = SFU_SESSION_REBIND_UNCHANGED;
  if (nominated) {
    session = sfu_session_table_get_or_create_by_ufrag(w->sessions, &pkt->peer_addr, pkt->peer_addr_len, client_ufrag, true, &rebind_result);
  } else {
    session = sfu_session_table_find_by_ufrag(w->sessions, client_ufrag);
  }
  if (!session) {
    if (!nominated) {
      SFU_LOG_DEBUG("worker %u: non-nominated check for ufrag=%s; waiting for USE-CANDIDATE", w->worker_index, client_ufrag);
      return;
    }
    if (rebind_result == SFU_SESSION_REBIND_REJECTED) {
      SFU_LOG_WARN("worker %u: authenticated ICE rebind rejected for ufrag=%s target=%s:%u", w->worker_index, client_ufrag, ip, port);
    } else {
      SFU_LOG_ERROR("worker %u: could not create/find session for %s:%u to bind room", w->worker_index, ip, port);
    }
    return;
  }
  if (!sfu_session_accepts_work(session)) {
    SFU_LOG_DEBUG("worker %u: session for ufrag=%s is closing; skipping room bind", w->worker_index, client_ufrag);
    sfu_session_release(session);
    return;
  }

  bool role_changed = false;
  bool media_changed = false;
  bool applied_answer = false;
  if (route.pending_generation != 0) {
    applied_answer = sfu_routing_table_reconcile_answer(w->routing_table, client_ufrag, route.room, route.fd, route.pending_generation, session, &role_changed,
                                                        &media_changed);
  }
  if (!applied_answer) {
    sfu_routing_snapshot_t latest;
    if (sfu_routing_table_lookup_route(w->routing_table, client_ufrag, w->worker_index, &latest) && latest.room == route.room && latest.fd == route.fd &&
        latest.pending_generation != 0 && latest.pending_generation != route.pending_generation) {
      applied_answer = sfu_routing_table_reconcile_answer(w->routing_table, client_ufrag, latest.room, latest.fd, latest.pending_generation, session,
                                                          &role_changed, &media_changed);
    }
  }
  if (applied_answer) {
    SFU_LOG_INFO("worker %u: applied deferred answer for ufrag=%s generation=%u peer_id=%u", w->worker_index, client_ufrag,
                 atomic_load_explicit(&session->applied_answer_generation, memory_order_acquire), session->peer_id);
  }

  bool newly_bound = false;
  if (!session->room) {
    bool has_applied_answer = atomic_load_explicit(&session->applied_answer_generation, memory_order_acquire) != 0 && session->fd == route.fd;
    if (!applied_answer && !has_applied_answer) {
      SFU_LOG_DEBUG("worker %u: authenticated STUN for ufrag=%s has no applied answer yet; deferring room publication", w->worker_index, client_ufrag);
      sfu_session_release(session);
      return;
    }

    sfu_routing_snapshot_t current_route;
    if (!sfu_routing_table_lookup_route(w->routing_table, client_ufrag, w->worker_index, &current_route) || current_route.room != route.room ||
        current_route.fd != route.fd) {
      SFU_LOG_INFO("worker %u: signaling route disappeared before binding ufrag=%s; skipping room publication", w->worker_index, client_ufrag);
      sfu_session_release(session);
      return;
    }

    SFU_LOG_INFO("worker %u: bound session %s:%u (ufrag=%s) to room_id=%" PRIu64, w->worker_index, ip, port, client_ufrag, route.room->room_id);
    room_add_peer(route.room, session);
    newly_bound = session->room == route.room;
    if (newly_bound) {
      sfu_signaling_notify_peer_admitted(route.room, session);
    }
  } else if (session->room != route.room) {
    SFU_LOG_WARN("worker %u: session ufrag=%s belongs to another room; rejecting route bind", w->worker_index, client_ufrag);
    sfu_session_release(session);
    return;
  }

  pthread_mutex_lock(&session->answer_lock);
  pthread_mutex_lock(&session->ingress_lock);
  uint16_t previous_worker = sfu_session_owner_worker(session);
  bool transfer_owner =
      nominated && (rebind_result == SFU_SESSION_REBIND_APPLIED || session->state != SFU_SESSION_ESTABLISHED || previous_worker == SFU_SESSION_OWNER_NONE);
  if (transfer_owner && previous_worker != w->worker_index) {
    if (!sfu_worker_register_session(w, session)) {
      SFU_LOG_ERROR("worker %u: failed to retain peer %u in local registry", w->worker_index, session->peer_id);
      transfer_owner = false;
    } else {
      sfu_session_set_owner_worker(session, (uint16_t)w->worker_index);
      if (previous_worker != SFU_SESSION_OWNER_NONE && previous_worker < w->scheduler->worker_count) {
        sfu_worker_unregister_session(&w->scheduler->workers[previous_worker], session);
      }
    }
  }
  uint16_t current_worker = sfu_session_owner_worker(session);
  pthread_mutex_unlock(&session->ingress_lock);
  pthread_mutex_unlock(&session->answer_lock);
  if (rebind_result == SFU_SESSION_REBIND_APPLIED) {
    SFU_LOG_INFO("worker %u: authenticated ICE address rebind applied ufrag=%s peer_id=%u target=%s:%u worker=%u->%u state=%d", w->worker_index, client_ufrag,
                 session->peer_id, ip, port, previous_worker, current_worker, session->state);
  }

  if (session->room && (newly_bound || role_changed || media_changed)) {
    room_refresh_peer_streams((sfu_room_t *)session->room, session);
  }

  sfu_session_release(session);
}

#define SFU_DTLS_RESTART_TIMEOUT_MS 15000u
#define SFU_SRTP_PREVIOUS_GRACE_MS 3000u

static void clear_pending_dtls(sfu_peer_session_t *session) {
  sfu_dtls_conn_destroy(&session->cold->pending_dtls);
  memset(&session->cold->pending_dtls, 0, sizeof(session->cold->pending_dtls));
  memset(session->cold->pending_client_random, 0, sizeof(session->cold->pending_client_random));
  session->cold->pending_dtls_started_ms = 0;
  session->cold->pending_dtls_active = false;
}

static void handle_dtls(sfu_worker_t *w, sfu_packet_t *pkt) {
  char ip[64];
  uint16_t port;
  format_peer_endpoint(&pkt->peer_addr, ip, &port);

  sfu_peer_session_t *session = sfu_session_table_find(w->sessions, &pkt->peer_addr, pkt->peer_addr_len);
  if (!session) {
    SFU_LOG_DEBUG("worker %u: dropping DTLS from unknown/closed peer %s:%u", w->worker_index, ip, port);
    return;
  }

  if (!sfu_session_accepts_work(session)) {
    SFU_LOG_DEBUG("worker %u: dropping DTLS for closing session %s:%u", w->worker_index, ip, port);
    sfu_session_release(session);
    return;
  }

  pthread_mutex_lock(&session->answer_lock);
  if (session->cold->addr_len != pkt->peer_addr_len || memcmp(&session->cold->addr, &pkt->peer_addr, pkt->peer_addr_len) != 0) {
    pthread_mutex_unlock(&session->answer_lock);
    sfu_session_release(session);
    return;
  }

  uint64_t now_ms = sfu_now_ms();
  if (session->cold->pending_dtls_active && now_ms - session->cold->pending_dtls_started_ms > SFU_DTLS_RESTART_TIMEOUT_MS) {
    clear_pending_dtls(session);
    sfu_metric_inc("dtls_restart_timeout");
  }

  uint8_t client_random[32];
  bool is_client_hello = sfu_dtls_extract_client_hello_random(pkt->data, pkt->len, client_random);
  if (session->state == SFU_SESSION_ESTABLISHED && is_client_hello && !session->cold->pending_dtls_active) {
    if (session->cold->active_client_random_valid && memcmp(client_random, session->cold->active_client_random, sizeof(client_random)) == 0) {
      sfu_metric_inc("dtls_restart_duplicate");
#ifdef SFU_DIAG_LOG
      char client_random_hex[65];
      for (size_t _dr = 0; _dr < 32; _dr++) {
        snprintf(client_random_hex + _dr * 2, 3, "%02x", (unsigned)client_random[_dr]);
      }
      SFU_LOG_WARN("worker %u: [DTLS DUPLICATE] peer=%u user_id=%" PRId64 " ufrag=%s %s:%u generation=%u client_random=%s", w->worker_index, session->peer_id,
                   session->user_id, session->cold->ufrag[0] ? session->cold->ufrag : "(none)", ip, port, (unsigned)session->cold->transport_generation,
                   client_random_hex);
#endif
      pthread_mutex_unlock(&session->answer_lock);
      sfu_session_release(session);
      return;
    }
    if (sfu_dtls_conn_init(&session->cold->pending_dtls, w->sessions->dtls_ctx) != 0) {
      sfu_metric_inc("dtls_restart_failed");
      pthread_mutex_unlock(&session->answer_lock);
      sfu_session_release(session);
      return;
    }
    memcpy(session->cold->pending_client_random, client_random, sizeof(client_random));
    session->cold->pending_dtls_started_ms = now_ms;
    session->cold->pending_dtls_active = true;
    sfu_metric_inc("dtls_restart_detected");
  } else if (session->state != SFU_SESSION_ESTABLISHED && is_client_hello && !session->cold->active_client_random_valid) {
    memcpy(session->cold->active_client_random, client_random, sizeof(client_random));
    session->cold->active_client_random_valid = true;
  }

  bool restarting = session->cold->pending_dtls_active;
  sfu_dtls_conn_t *dtls = restarting ? &session->cold->pending_dtls : &session->cold->dtls;
  sfu_dtls_feed_status_t status = sfu_dtls_conn_feed(dtls, pkt->data, pkt->len, NULL, NULL);

  uint8_t out[4096];
  size_t out_len = sfu_dtls_conn_drain_output(dtls, out, sizeof(out));
  if (out_len > 0) {
    send_raw(w, out, out_len, &pkt->peer_addr, pkt->peer_addr_len);
  }

  switch (status) {
    case SFU_DTLS_FEED_ESTABLISHED:
      if (restarting) {
        sfu_srtp_ctx_t next_srtp;
        memset(&next_srtp, 0, sizeof(next_srtp));
        if (sfu_srtp_ctx_init_from_dtls(&next_srtp, dtls->srtp_keying_material, dtls->srtp_profile_id, true) != 0) {
          clear_pending_dtls(session);
          sfu_metric_inc("dtls_restart_failed");
          break;
        }

        pthread_mutex_lock(&session->crypto_lock);
        sfu_srtp_ctx_destroy(&session->previous_srtp);
        session->previous_srtp = session->srtp;
        session->srtp = next_srtp;
        memset(&next_srtp, 0, sizeof(next_srtp));
        session->previous_srtp_expires_ms = now_ms + SFU_SRTP_PREVIOUS_GRACE_MS;
        pthread_mutex_unlock(&session->crypto_lock);

        sfu_dtls_conn_destroy(&session->cold->dtls);
        session->cold->dtls = session->cold->pending_dtls;
        memset(&session->cold->pending_dtls, 0, sizeof(session->cold->pending_dtls));
        memcpy(session->cold->active_client_random, session->cold->pending_client_random, sizeof(session->cold->active_client_random));
        session->cold->active_client_random_valid = true;
        session->cold->pending_dtls_active = false;
        session->cold->pending_dtls_started_ms = 0;
        session->cold->transport_generation++;
        sfu_metric_inc("dtls_restart_established");

#ifdef SFU_DIAG_LOG
        char client_random_hex[65];
        for (size_t _cr = 0; _cr < 32; _cr++) {
          snprintf(client_random_hex + _cr * 2, 3, "%02x", (unsigned)session->cold->active_client_random[_cr]);
        }
        SFU_LOG_INFO("worker %u: DTLS transport restarted for peer %u user_id=%" PRId64 " ufrag=%s generation=%u at %s:%u profile=0x%lx client_random=%s",
                     w->worker_index, session->peer_id, session->user_id, session->cold->ufrag[0] ? session->cold->ufrag : "(none)",
                     (unsigned)session->cold->transport_generation, ip, port, session->cold->dtls.srtp_profile_id, client_random_hex);
#endif
      } else if (session->state != SFU_SESSION_ESTABLISHED) {
        pthread_mutex_lock(&session->crypto_lock);
        int srtp_rc = sfu_srtp_ctx_init_from_dtls(&session->srtp, dtls->srtp_keying_material, dtls->srtp_profile_id, true);
        pthread_mutex_unlock(&session->crypto_lock);
        if (srtp_rc != 0) {
          SFU_LOG_ERROR("worker %u: failed to derive SRTP keys after DTLS handshake for %s:%u", w->worker_index, ip, port);
          session->state = SFU_SESSION_FAILED;
          break;
        }

        session->cold->transport_generation = 1;
        session->state = SFU_SESSION_ESTABLISHED;

#ifdef SFU_DIAG_LOG
        char client_random_hex[65];
        for (size_t _cr2 = 0; _cr2 < 32; _cr2++) {
          snprintf(client_random_hex + _cr2 * 2, 3, "%02x", (unsigned)session->cold->active_client_random[_cr2]);
        }
        SFU_LOG_INFO("worker %u: DTLS established, SRTP sessions ready for peer=%u user_id=%" PRId64
                     " ufrag=%s at %s:%u gen=%u (room %s) profile=0x%lx client_random=%s",
                     w->worker_index, session->peer_id, session->user_id, session->cold->ufrag[0] ? session->cold->ufrag : "(none)", ip, port,
                     (unsigned)session->cold->transport_generation, session->room ? "BOUND" : "STILL UNBOUND -- media will be dropped until it binds",
                     dtls->srtp_profile_id, client_random_hex);
#endif

        if (session->room) {
          sfu_signaling_schedule_pending_peer(session);
        }
      }
      break;
    case SFU_DTLS_FEED_IN_PROGRESS:
      if (!restarting) {
        session->state = SFU_SESSION_DTLS_HANDSHAKING;
      }
      break;
    case SFU_DTLS_FEED_ERROR:
      if (restarting) {
        clear_pending_dtls(session);
        sfu_metric_inc("dtls_restart_failed");
      } else {
        session->state = SFU_SESSION_FAILED;
        SFU_LOG_WARN("worker %u: DTLS handshake failed for peer %s:%u", w->worker_index, ip, port);
      }
      break;
  }

  pthread_mutex_unlock(&session->answer_lock);
  sfu_session_release(session);
}

void sfu_dispatch_packet(sfu_worker_t *w, sfu_packet_t *pkt) {
  char ip[64];
  uint16_t port;
  format_peer_endpoint(&pkt->peer_addr, ip, &port);

  if (sfu_stun_is_stun_packet(pkt->data, pkt->len)) {
    SFU_LOG_DEBUG("worker %u: Identified STUN packet from %s:%u", w->worker_index, ip, port);
    handle_stun(w, pkt);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  if (sfu_dtls_is_dtls_packet(pkt->data, pkt->len)) {
    SFU_LOG_DEBUG("worker %u: Identified DTLS packet from %s:%u", w->worker_index, ip, port);
    handle_dtls(w, pkt);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  sfu_ingress_process(w, pkt);
}
