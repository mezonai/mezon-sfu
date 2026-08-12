#ifndef SFU_PROTOCOL_SIGNALING_H
#define SFU_PROTOCOL_SIGNALING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <uv.h>
#include "room/room_registry.h"
#include "runtime/routing_context.h"
#include "transport/stun/stun.h"

typedef struct sfu_signaling_server {
  int listen_fd;
  atomic_bool running;
  pthread_t thread;
  uv_async_t async_waker;
  const sfu_ice_credentials_t *ice_creds;
  const sfu_dtls_ctx_t *dtls_ctx;
  sfu_session_table_t *sessions;
  sfu_room_registry_t *room_registry;
  sfu_routing_table_t *routing_table;
  char media_host[64];
  uint16_t media_port;
  uint16_t _pad16;
} sfu_signaling_server_t;

typedef struct sfu_client_conn {
  char pending_answer_sdp[SFU_SIGNALING_SDP_CAP];
  uv_poll_t poll_handle;
  uv_timer_t answer_retry_timer;
  sfu_signaling_server_t *server;
  sfu_room_t *joined_room;
  uint64_t joined_room_id;
  int64_t user_id;
  int fd;
  int pending_answer_sdp_len;
  int answer_retry_count;
  uint8_t ip_detected_from_header;
  char peer_ip[64];
  char client_ufrag[32];
  bool handshake_done;
  bool is_audience;
} sfu_client_conn_t;

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port, const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds, const sfu_dtls_ctx_t *dtls_ctx, sfu_session_table_t *sessions,
                               sfu_room_registry_t *room_registry, sfu_routing_table_t *routing_table);
void sfu_signaling_server_stop(sfu_signaling_server_t *s);
void sfu_signaling_trigger_renegotiation(sfu_room_t *room);
void sfu_signaling_generate_turn_credentials(const char *secret, const char *username_suffix, char *out_username, size_t user_sz, char *out_password,
                                             size_t pass_sz, uint32_t ttl_seconds);
uint32_t generate_unique_id(void);

#endif /* SFU_PROTOCOL_SIGNALING_H */
