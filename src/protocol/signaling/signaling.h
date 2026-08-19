#ifndef SFU_PROTOCOL_SIGNALING_H
#define SFU_PROTOCOL_SIGNALING_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <uv.h>
#include "protocol/websocket/ws.h"
#include "room/room_registry.h"
#include "runtime/routing_context.h"
#include "transport/stun/stun.h"

#define SFU_SIGNALING_PING_INTERVAL_MS 10000u
#define SFU_SIGNALING_IDLE_TIMEOUT_MS 20000u
#define SFU_RENEGOTIATION_QUEUE_CAP 2048u
#define SFU_RENEGOTIATION_DEBOUNCE_MS 15u
#define SFU_RENEGOTIATION_MAX_DELAY_MS 50u
#define SFU_RENEGOTIATION_RETRY_MAX_MS 500u
#define SFU_MEMBERSHIP_QUEUE_CAP 2048u

typedef struct sfu_renegotiation_queue {
  sfu_peer_session_t *items[SFU_RENEGOTIATION_QUEUE_CAP];
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  pthread_mutex_t lock;
} sfu_renegotiation_queue_t;

typedef struct sfu_membership_queue {
  sfu_peer_session_t *items[SFU_MEMBERSHIP_QUEUE_CAP];
  sfu_peer_session_t *media_items[SFU_MEMBERSHIP_QUEUE_CAP];
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  uint32_t media_head;
  uint32_t media_tail;
  uint32_t media_count;
  pthread_mutex_t lock;
} sfu_membership_queue_t;

typedef struct sfu_signaling_scratch {
  char *recv;
  char *sdp;
  char *json;
} sfu_signaling_scratch_t;

typedef struct sfu_signaling_server {
  int listen_fd;
  atomic_bool running;
  pthread_t thread;
  uv_async_t async_waker;
  uv_async_t renegotiation_waker;
  uv_timer_t renegotiation_timer;
  bool renegotiation_timer_inited;
  sfu_renegotiation_queue_t renegotiation_queue;
  sfu_membership_queue_t membership_queue;
  const sfu_ice_credentials_t *ice_creds;
  const sfu_dtls_ctx_t *dtls_ctx;
  sfu_session_table_t *sessions;
  sfu_room_registry_t *room_registry;
  sfu_routing_table_t *routing_table;
  struct sfu_client_conn *connections_head;
  sfu_signaling_scratch_t scratch;
  char media_host[64];
  uint16_t media_port;
  uint16_t _pad16;
} sfu_signaling_server_t;

typedef struct sfu_client_conn {
  uv_poll_t poll_handle;
  uv_timer_t keepalive_timer;
  sfu_signaling_server_t *server;
  struct sfu_client_conn *registry_prev;
  struct sfu_client_conn *registry_next;
  sfu_room_t *joined_room;
  uint64_t joined_room_id;
  uint64_t last_activity_ms;
  int64_t user_id;
  int fd;
  uint8_t ip_detected_from_header;
  uint8_t handles_open;
  char peer_ip[64];
  char client_ufrag[32];
  sfu_ws_read_state_t ws_read;
  bool handshake_done;
  bool is_audience;
  bool disconnecting;
  bool keepalive_inited;
  bool in_registry;
} sfu_client_conn_t;

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port, const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds, const sfu_dtls_ctx_t *dtls_ctx, sfu_session_table_t *sessions,
                               sfu_room_registry_t *room_registry, sfu_routing_table_t *routing_table);
void sfu_signaling_server_stop(sfu_signaling_server_t *s);
void sfu_signaling_trigger_peer_renegotiation(sfu_peer_session_t *session);
void sfu_signaling_schedule_pending_peer(sfu_peer_session_t *session);
void sfu_signaling_notify_peer_admitted(sfu_room_t *room, sfu_peer_session_t *peer);
void sfu_signaling_notify_media_state(sfu_peer_session_t *peer);
void sfu_signaling_generate_turn_credentials(const char *secret, const char *username_suffix, char *out_username, size_t user_sz, char *out_password,
                                             size_t pass_sz, uint32_t ttl_seconds);
uint32_t generate_unique_id(void);

#endif /* SFU_PROTOCOL_SIGNALING_H */
