#ifndef SFU_RUNTIME_WORKER_H
#define SFU_RUNTIME_WORKER_H

#include <pthread.h>
#include <stdint.h>

#include "memory/packet_pool.h"
#include "net/io_uring.h"
#include "peer/session.h"
#include "room/room_registry.h"
#include "runtime/fanout.h"
#include "runtime/routing_context.h"
#include "transport/stun/stun.h"
#include "util/ringbuffer.h"

typedef struct sfu_worker {
  int core_id;           /* which CPU core this thread is pinned to */
  uint32_t worker_index; /* stable 0..worker_count-1 identity, used
                          * for room/mesh addressing -- distinct
                          * from core_id so pinning policy can
                          * change independently of routing */
  sfu_ring_t send_ring;
  sfu_spsc_ring_t inbox;                 /* dispatcher -> this worker */
  sfu_spsc_ring_t release_to_dispatcher; /* this worker -> dispatcher, kernel
                                          * buffer returns */

  sfu_packet_pool_t *pp;                  /* shared with the dispatcher, not owned */
  sfu_room_registry_t *room_registry;     /* shared peer registry, not owned */
  sfu_fanout_mesh_t *mesh;                /* shared cross-worker fan-out mesh, not owned */
  sfu_session_table_t *sessions;          /* shared ICE/DTLS session table, not owned */
  sfu_routing_table_t *routing_table;     /* Shared non-global routing/ownership context, not owned */
  const sfu_ice_credentials_t *ice_creds; /* shared local ICE ufrag/pwd, not owned */

  pthread_t thread;
  int fd; /* same UDP socket the dispatcher recvs on */
} sfu_worker_t;

int sfu_worker_init(sfu_worker_t *w, int core_id, uint32_t worker_index, int fd, sfu_packet_pool_t *pp, sfu_room_registry_t *room_registry,
                    sfu_fanout_mesh_t *mesh, sfu_session_table_t *sessions, sfu_routing_table_t *routing_table, const sfu_ice_credentials_t *ice_creds,
                    uint32_t inbox_capacity, int send_bgid);

/*
 * Forwards one RTP/RTCP packet to every other room member per the
 * current (sender-address-based) room registry -- local subscribers
 * get a direct send_zc, remote-core subscribers go through the fan-out
 * mesh. Consumes exactly one reference on pkt. This is the same logic
 * that previously ran unconditionally on every received packet; now
 * pipeline/dispatch.c only calls it once a peer's session has actually
 * completed the DTLS handshake.
 */
void sfu_room_forward_packet(sfu_worker_t *w, sfu_packet_t *pkt);
void sfu_worker_destroy(sfu_worker_t *w);

/* Spawns the worker's thread, which pins itself to core_id and runs
 * until sfu_shutdown_requested() is observed. */
int sfu_worker_start(sfu_worker_t *w);
void sfu_worker_join(sfu_worker_t *w);

#endif /* SFU_RUNTIME_WORKER_H */
