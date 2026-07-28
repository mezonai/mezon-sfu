#ifndef SFU_PROTOCOL_SIGNALING_H
#define SFU_PROTOCOL_SIGNALING_H

#include <pthread.h>
#include <stdint.h>
#include <uv.h>
#include "room/room_registry.h"
#include "runtime/routing_context.h"
#include "transport/stun/stun.h"

typedef struct sfu_signaling_server {
  int listen_fd;
  pthread_t thread;
  volatile int running;
  uv_async_t async_waker;
  char media_host[64];
  uint16_t media_port;
  const sfu_ice_credentials_t *ice_creds; /* shared, not owned */
  const sfu_dtls_ctx_t *dtls_ctx;         /* shared, not owned */
  sfu_session_table_t *sessions;
  sfu_room_registry_t *room_registry;
  sfu_routing_table_t *routing_table;
} sfu_signaling_server_t;


typedef struct {
  uv_poll_t poll_handle;
  int fd;
  bool handshake_done;
  char peer_ip[64];
  int ip_detected_from_header;
  sfu_room_t *joined_room;
  uint64_t joined_room_id;
  char client_ufrag[32];
  sfu_signaling_server_t *server;
  sfu_peer_session_t *session;
} sfu_client_conn_t;

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port, const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds, const sfu_dtls_ctx_t *dtls_ctx, sfu_session_table_t *sessions,
                               sfu_room_registry_t *room_registry, sfu_routing_table_t *routing_table);
void sfu_signaling_server_stop(sfu_signaling_server_t *s);
void sfu_signaling_trigger_renegotiation(sfu_room_t *room, const char *exclude_ufrag);

#endif /* SFU_PROTOCOL_SIGNALING_H */
