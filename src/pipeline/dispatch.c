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
#include "transport/dtls/dtls.h"
#include "transport/srtp/srtp.h"
#include "transport/stun/stun.h"
#include "util/log.h"

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
  sfu_room_t *room = NULL;
  int matched_signaling_fd = -1;
  bool has_pending_answer = false;
  uint32_t pending_audio_ssrc = 0, pending_video_ssrc = 0, pending_rtx_ssrc = 0;
  uint8_t pending_video_pt = 0, pending_rtx_pt = 0;

  if (have_ufrag) {
    pthread_mutex_lock(&w->routing_table->mutex);

    sfu_routing_entry_t *match = NULL;
    for (int i = 0; i < w->routing_table->count; i++) {
      if (strcmp(w->routing_table->entries[i].ufrag, client_ufrag) == 0) {
        match = &w->routing_table->entries[i];
        break;
      }
    }

    if (match) {
      if (match->has_owner && match->worker_index != w->worker_index) {
        SFU_LOG_INFO("worker %u: Migrating ufrag=%s from worker %u (NAT path changed to %s:%u)", w->worker_index, client_ufrag, match->worker_index, ip, port);
        match->worker_index = w->worker_index;
      }

      if (!match->has_owner) {
        match->worker_index = w->worker_index;
        match->has_owner = true;
        SFU_LOG_INFO("worker %u: Claimed ownership of ufrag=%s", w->worker_index, client_ufrag);
      }

      room = (sfu_room_t *)match->room;
      matched_signaling_fd = match->fd;
      if (match->has_pending_answer) {
        has_pending_answer = true;
        pending_audio_ssrc = match->pending_audio_ssrc;
        pending_video_ssrc = match->pending_video_ssrc;
        pending_rtx_ssrc = match->pending_rtx_ssrc;
        pending_video_pt = match->pending_video_pt;
        pending_rtx_pt = match->pending_rtx_pt;
        match->has_pending_answer = false;
      }
    }
    pthread_mutex_unlock(&w->routing_table->mutex);
  } else {
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

  SFU_LOG_DEBUG("worker %u: Responding to STUN Binding Request from %s:%u", w->worker_index, ip, port);

  send_raw(w, response, response_len, &pkt->peer_addr, pkt->peer_addr_len);

  if (have_ufrag) {
    if (room) {
      /* Both lookups return a caller pin (+1 ref) that this function releases
       * exactly once on every path below. */
      sfu_peer_session_t *session = NULL;
      if (client_ufrag[0] != '\0') {
        session = sfu_session_table_find_by_ufrag(w->sessions, client_ufrag);
      }

      if (session) {
        bool addr_changed = !(session->cold->addr_len == pkt->peer_addr_len && memcmp(&session->cold->addr, &pkt->peer_addr, pkt->peer_addr_len) == 0);
        if (addr_changed) {
          if (session->state == SFU_SESSION_ESTABLISHED) {
            SFU_LOG_DEBUG("worker %u: ufrag=%s STUN from alternate candidate %s:%u (session already established at different addr, not rebinding)",
                          w->worker_index, client_ufrag, ip, port);
          } else if (!sfu_session_table_rebind_addr(w->sessions, session, &pkt->peer_addr, pkt->peer_addr_len)) {
            SFU_LOG_DEBUG("worker %u: ufrag=%s rebind rejected (session closing or not a table member)", w->worker_index, client_ufrag);
          }
        }
      } else {
        session = sfu_session_table_get_or_create(w->sessions, &pkt->peer_addr, pkt->peer_addr_len);
      }
      if (!session) {
        SFU_LOG_ERROR("worker %u: could not create/find session for %s:%u to bind room", w->worker_index, ip, port);
      } else if (!sfu_session_accepts_work(session)) {
        /* Session is closing/closed: do not bind, index, or mutate it. */
        SFU_LOG_DEBUG("worker %u: session for ufrag=%s is closing; skipping room bind", w->worker_index, client_ufrag);
      } else {
        if (session->cold->ufrag[0] == '\0') {
          strncpy(session->cold->ufrag, client_ufrag, sizeof(session->cold->ufrag) - 1);
          session->cold->ufrag[sizeof(session->cold->ufrag) - 1] = '\0';
          if (!sfu_session_table_index_ufrag(w->sessions, session)) {
            SFU_LOG_WARN("worker %u: ufrag index rejected for closing session %s", w->worker_index, client_ufrag);
          }
        }

        if (!session->room) {
          SFU_LOG_INFO("worker %u: bound session %s:%u (ufrag=%s) to room_id=%" PRIu64, w->worker_index, ip, port, client_ufrag, room->room_id);
          room_add_peer(room, session);
          session->fd = matched_signaling_fd;

          if (has_pending_answer) {
            session->uplink_audio.ssrc = pending_audio_ssrc;
            session->uplink_audio.active = (pending_audio_ssrc != 0);
            session->uplink_video.ssrc = pending_video_ssrc;
            session->uplink_video.rtx_ssrc = pending_rtx_ssrc;
            session->uplink_video.active = (pending_video_ssrc != 0);
            session->uplink_video.payload_type = pending_video_pt;
            session->uplink_video.rtx_payload_type = pending_rtx_pt;
            for (int pi = 0; pi < 128; pi++) {
              session->pt_map[pi] = (uint8_t)pi;
            }
            if (pending_video_pt != 0) {
              session->pt_map[96] = pending_video_pt;
            }
            if (pending_rtx_pt != 0) {
              session->pt_map[97] = pending_rtx_pt;
            }
            SFU_LOG_INFO("worker %u: applied deferred answer for ufrag=%s: audio_ssrc=%u video_ssrc=%u rtx_ssrc=%u", w->worker_index, client_ufrag,
                         pending_audio_ssrc, pending_video_ssrc, pending_rtx_ssrc);
          }
        }

        if (session->worker_id == UINT16_MAX) {
          session->worker_id = w->worker_index;
          SFU_LOG_INFO("worker %u: session ufrag=%s assigned to worker %u", w->worker_index, session->cold->ufrag, session->worker_id);
        } else if (session->worker_id != w->worker_index) {
          SFU_LOG_INFO("worker %u: session ufrag=%s worker ownership migrating %u -> %u", w->worker_index, session->cold->ufrag, session->worker_id,
                       w->worker_index);
          session->worker_id = w->worker_index;
        }
      }
      if (session) {
        sfu_session_release(session);
      }
    } else {
      SFU_LOG_DEBUG(
          "worker %u: no room registered yet for ufrag=%s (from %s:%u) -- "
          "offer may not have reached signaling yet, will retry on next STUN",
          w->worker_index, client_ufrag, ip, port);
    }
  }
}

static void handle_dtls(sfu_worker_t *w, sfu_packet_t *pkt) {
  char ip[64];
  uint16_t port;
  format_peer_endpoint(&pkt->peer_addr, ip, &port);

  /* Caller pin: released at the end of this handler on every path. */
  sfu_peer_session_t *session = sfu_session_table_get_or_create(w->sessions, &pkt->peer_addr, pkt->peer_addr_len);
  if (!session) {
    SFU_LOG_ERROR("worker %u: CRITICAL! Session table full or rejected registration for %s:%u", w->worker_index, ip, port);
    return;
  }

  if (!sfu_session_accepts_work(session)) {
    SFU_LOG_DEBUG("worker %u: dropping DTLS for closing session %s:%u", w->worker_index, ip, port);
    sfu_session_release(session);
    return;
  }

  SFU_LOG_INFO("worker %u: Feeding %u bytes of DTLS data from %s:%u (current state: %d, room %s)", w->worker_index, pkt->len, ip, port, session->state,
               session->room ? "BOUND" : "unbound");

  sfu_dtls_feed_status_t status = sfu_dtls_conn_feed(&session->cold->dtls, pkt->data, pkt->len, NULL, NULL);

  uint8_t out[4096];
  size_t out_len = sfu_dtls_conn_drain_output(&session->cold->dtls, out, sizeof(out));
  if (out_len > 0) {
    send_raw(w, out, out_len, &pkt->peer_addr, pkt->peer_addr_len);
  }

  switch (status) {
    case SFU_DTLS_FEED_ESTABLISHED:
      if (session->state != SFU_SESSION_ESTABLISHED) {
        if (sfu_srtp_ctx_init_from_dtls(&session->srtp, session->cold->dtls.srtp_keying_material, session->cold->dtls.srtp_profile_id, true) != 0) {
          SFU_LOG_ERROR("worker %u: failed to derive SRTP keys after DTLS handshake for %s:%u", w->worker_index, ip, port);
          session->state = SFU_SESSION_FAILED;
          break;
        }

        session->state = SFU_SESSION_ESTABLISHED;
        SFU_LOG_INFO("worker %u: DTLS established, SRTP sessions ready for %s:%u (room %s)", w->worker_index, ip, port,
                     session->room ? "BOUND" : "STILL UNBOUND -- media will be dropped until it binds");

        if (session->room) {
          SFU_LOG_INFO("worker %u: SRTP secure pipeline verified. Dispatching room renegotiation for room context.", w->worker_index);
          sfu_signaling_trigger_renegotiation((sfu_room_t *)session->room);
        }
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
    SFU_LOG_INFO("worker %u: Identified DTLS packet from %s:%u", w->worker_index, ip, port);
    handle_dtls(w, pkt);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  sfu_ingress_process(w, pkt);
}
