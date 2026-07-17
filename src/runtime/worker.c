#include "runtime/worker.h"
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include "pipeline/dispatch.h"
#include "runtime/cpu.h"
#include "runtime/signal.h"
#include "util/log.h"

#define SFU_WORKER_SEND_SQ_ENTRIES 1024
#define SFU_WORKER_SEND_CQ_ENTRIES 2048
#define SFU_WORKER_REAP_BATCH 128
#define SFU_WORKER_IDLE_SLEEP_US 200

extern sfu_room_t *sfu_lookup_ip_room(const char *ip);

int sfu_worker_init(sfu_worker_t *w, int core_id, uint32_t worker_index, int fd, sfu_packet_pool_t *pp, sfu_room_registry_t *room_registry,
                    sfu_fanout_mesh_t *mesh, sfu_session_table_t *sessions, const sfu_ice_credentials_t *ice_creds, uint32_t inbox_capacity, int send_bgid) {
  memset(w, 0, sizeof(*w));
  w->core_id = core_id;
  w->worker_index = worker_index;
  w->fd = fd;
  w->pp = pp;
  w->room_registry = room_registry;
  w->mesh = mesh;
  w->sessions = sessions;
  w->ice_creds = ice_creds;

  if (sfu_spsc_ring_init(&w->inbox, inbox_capacity) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init inbox ring", worker_index);
    return -1;
  }

  if (sfu_spsc_ring_init(&w->release_to_dispatcher, SFU_RELEASE_QUEUE_CAPACITY) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init release queue", worker_index);
    sfu_spsc_ring_destroy(&w->inbox);
    return -1;
  }

  /* Send-only ring: no provided buffers, this worker never recvs. */
  if (sfu_ring_init(&w->send_ring, fd, SFU_WORKER_SEND_SQ_ENTRIES, SFU_WORKER_SEND_CQ_ENTRIES, 0, 0, send_bgid, false) != 0) {
    SFU_LOG_ERROR("worker %u: failed to init send ring", worker_index);
    sfu_spsc_ring_destroy(&w->release_to_dispatcher);
    sfu_spsc_ring_destroy(&w->inbox);
    return -1;
  }

  return 0;
}

void sfu_worker_destroy(sfu_worker_t *w) {
  sfu_ring_destroy(&w->send_ring);
  sfu_spsc_ring_destroy(&w->release_to_dispatcher);
  sfu_spsc_ring_destroy(&w->inbox);
}

/*
 * Real forward step. A publisher's ciphertext was encrypted with keys
 * only its own DTLS session knows -- every subscriber negotiated
 * independent keys of their own -- so this is necessarily a decrypt-
 * once, re-encrypt-per-subscriber relay, not a byte-for-byte fan-out.
 */
void sfu_room_forward_packet(sfu_worker_t *w, sfu_packet_t *pkt) {
  // Extract string representations of the source IP and port for tracking
  char peer_ip[64] = "unknown";
  uint16_t peer_port = 0;
  if (pkt->peer_addr.ss_family == AF_INET) {
    struct sockaddr_in *s4 = (struct sockaddr_in *)&pkt->peer_addr;
    inet_ntop(AF_INET, &s4->sin_addr, peer_ip, sizeof(peer_ip));
    peer_port = ntohs(s4->sin_port);
  } else if (pkt->peer_addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&pkt->peer_addr;
    inet_ntop(AF_INET6, &s6->sin6_addr, peer_ip, sizeof(peer_ip));
    peer_port = ntohs(s6->sin6_port);
  }

  // Find the sender's active session first
  sfu_peer_session_t *sender_session = sfu_session_table_find(w->sessions, &pkt->peer_addr, pkt->peer_addr_len);

  // DIAGNOSTIC LOG 1: Track if the session registry table even knows this socket address
  if (!sender_session) {
    SFU_LOG_WARN("worker %u: UNKNOWN PACKET! Received UDP data from unregistered endpoint %s:%u (size=%u). Dropping.", w->worker_index, peer_ip, peer_port,
                 pkt->len);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  // DIAGNOSTIC LOG 2: Verify the exact DTLS state machine value if handshake is incomplete
  if (sender_session->state != SFU_SESSION_ESTABLISHED) {
    SFU_LOG_WARN("worker %u: HANDSHAKE NOT READY! Packet from %s:%u dropped because session state is %d (Expected ESTABLISHED=%d)", w->worker_index, peer_ip,
                 peer_port, sender_session->state, SFU_SESSION_ESTABLISHED);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  // Validate that the session is bound to an active, initialized room
  sfu_room_t *active_room = sender_session->room;
  if (!active_room) {
    SFU_LOG_INFO("worker %u: Attempting lookup for incoming UDP packet from IP: %s", w->worker_index, peer_ip);

    active_room = sfu_lookup_ip_room(peer_ip);
    if (active_room) {
      SFU_LOG_INFO("worker %u: Match found! IP %s mapped to Room ID: %" PRIu64, w->worker_index, peer_ip, active_room->room_id);
      // Permanently bind this UDP session to the resolved room to prevent future lookups
      sender_session->room = active_room;
    } else {
      // DIAGNOSTIC LOG 3: Explicit log for when IP routing lookup returns absolutely nothing
      SFU_LOG_ERROR("worker %u: ROOM LOOKUP FAILED! No active room registry matches IP %s", w->worker_index, peer_ip);
    }
  }

  if (!active_room) {
    SFU_LOG_WARN("worker %u: packet received from session with no assigned room, dropping", w->worker_index);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }

  // Register peer presence inside their dynamic room
  sfu_room_touch_peer(active_room, &pkt->peer_addr, pkt->peer_addr_len, w->worker_index);

  bool is_rtcp = sfu_rtp_is_rtcp(pkt->data, pkt->len);
  int plain_len = (int)pkt->len;
  bool unprotected =
      is_rtcp ? sfu_srtp_unprotect_rtcp(&sender_session->srtp, pkt->data, &plain_len) : sfu_srtp_unprotect_rtp(&sender_session->srtp, pkt->data, &plain_len);

  // DIAGNOSTIC LOG 4: Check if decrypt keys match the incoming SRTP auth tags
  if (!unprotected) {
    SFU_LOG_ERROR("worker %u: SRTP DECRYPTION FAILED! Could not unprotect packet from %s:%u. Check DTLS keys.", w->worker_index, peer_ip, peer_port);
    sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
    return;
  }
  pkt->len = (uint32_t)plain_len; /* pkt->data now holds plaintext */

  // Retrieve subscribers belonging exclusively to this session's room
  sfu_peer_entry_t subs[SFU_ROOM_MAX_PEERS];
  uint32_t n = sfu_room_list_subscribers_excluding(active_room, &pkt->peer_addr, pkt->peer_addr_len, subs, SFU_ROOM_MAX_PEERS);

  for (uint32_t i = 0; i < n; i++) {
    sfu_peer_entry_t *sub = &subs[i];

    sfu_peer_session_t *sub_session = sfu_session_table_find(w->sessions, &sub->addr, sub->addr_len);
    if (!sub_session || sub_session->state != SFU_SESSION_ESTABLISHED) {
      continue; /* this subscriber hasn't finished its own handshake yet */
    }

    sfu_packet_t *enc = sfu_packet_pool_alloc(w->pp);
    if (!enc) {
      SFU_LOG_WARN("worker %u: packet pool exhausted, dropping one subscriber send", w->worker_index);
      continue;
    }
    if (pkt->len > enc->cap) {
      SFU_LOG_WARN("worker %u: plaintext too large to re-encrypt (%u > %u)", w->worker_index, pkt->len, enc->cap);
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      continue;
    }
    memcpy(enc->data, pkt->data, pkt->len);
    int enc_len = (int)pkt->len;
    bool protected_ = is_rtcp ? sfu_srtp_protect_rtcp(&sub_session->srtp, enc->data, &enc_len, enc->cap)
                              : sfu_srtp_protect_rtp(&sub_session->srtp, enc->data, &enc_len, enc->cap);
    if (!protected_) {
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      continue;
    }
    enc->len = (uint32_t)enc_len;

    if (sub->worker_id == w->worker_index) {
      if (sfu_ring_queue_send_zc(&w->send_ring, enc, (const struct sockaddr *)&sub->addr, sub->addr_len) != 0) {
        SFU_LOG_WARN("worker %u: local send SQ full, dropping to one subscriber", w->worker_index);
      }
      /* Drop our allocation reference; the in-flight send (if it
       * was queued) holds its own via the internal retain. */
      sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
    } else {
      /* enc is freshly allocated and uniquely owned by this
       * subscriber's encryption -- unlike the old shared-
       * ciphertext design, the mesh job just takes over this one
       * reference wholesale, no extra retain needed. */
      if (!sfu_fanout_mesh_enqueue(w->mesh, w->worker_index, sub->worker_id, enc, &sub->addr, sub->addr_len)) {
        sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, enc);
      }
    }
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, pkt);
}

/* Consumes one job handed to this worker by another worker's fan-out */
static void handle_fanout_job(void *user_data, sfu_fanout_job_t *job) {
  sfu_worker_t *w = (sfu_worker_t *)user_data;

  if (sfu_ring_queue_send_zc(&w->send_ring, job->pkt, (const struct sockaddr *)&job->dst, job->dst_len) != 0) {
    SFU_LOG_WARN("worker %u: remote-fanout send SQ full, dropping", w->worker_index);
  }

  sfu_worker_release_packet(w->pp, &w->release_to_dispatcher, job->pkt);
  sfu_fanout_mesh_free_job(w->mesh, job);
}

static void *worker_thread_main(void *arg) {
  sfu_worker_t *w = (sfu_worker_t *)arg;
  sfu_pin_current_thread_to_core(w->core_id);

  SFU_LOG_INFO("worker %u started (core %d)", w->worker_index, w->core_id);

  while (!sfu_shutdown_requested()) {
    bool did_work = false;

    /* packets received directly from the dispatcher (this worker's own inbox). */
    void *item;
    int drained = 0;
    while (drained < SFU_WORKER_REAP_BATCH && sfu_spsc_ring_pop(&w->inbox, &item)) {
      sfu_dispatch_packet(w, (sfu_packet_t *)item);
      drained++;
      did_work = true;
    }

    /* jobs handed to this worker by *other* workers' fan-out */
    unsigned fanned = sfu_fanout_mesh_drain(w->mesh, w->worker_index, SFU_WORKER_REAP_BATCH, handle_fanout_job, w);
    if (fanned > 0) {
      did_work = true;
    }

    if (drained > 0 || fanned > 0) {
      sfu_ring_submit(&w->send_ring);
    }

    unsigned reaped = sfu_ring_reap(&w->send_ring, SFU_WORKER_REAP_BATCH, w->pp, &w->release_to_dispatcher, NULL, NULL, w);
    if (reaped > 0) {
      did_work = true;
    }

    if (!did_work) {
      usleep(SFU_WORKER_IDLE_SLEEP_US);
    }
  }

  SFU_LOG_INFO("worker %u shutting down", w->worker_index);
  return NULL;
}

int sfu_worker_start(sfu_worker_t *w) {
  int rc = pthread_create(&w->thread, NULL, worker_thread_main, w);
  if (rc != 0) {
    SFU_LOG_ERROR("worker %d: pthread_create failed: %d", w->core_id, rc);
    return -1;
  }
  return 0;
}

void sfu_worker_join(sfu_worker_t *w) { pthread_join(w->thread, NULL); }
