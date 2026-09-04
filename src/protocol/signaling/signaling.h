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
#define SFU_RENEGOTIATION_QUEUE_CAP SFU_SESSION_TABLE_MAX
#define SFU_RENEGOTIATION_DEBOUNCE_MS 15u
#define SFU_RENEGOTIATION_MAX_DELAY_MS 50u
#define SFU_RENEGOTIATION_RETRY_MAX_MS 500u
#define SFU_MEMBERSHIP_QUEUE_CAP 2048u

typedef enum sfu_membership_event_kind {
  SFU_MEMBERSHIP_JOIN = 1,
  SFU_MEMBERSHIP_LEAVE = 2,
} sfu_membership_event_kind_t;

typedef struct sfu_membership_member {
  uint32_t peer_id;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint32_t mid_screen;
  uint32_t remote_slot;
  uint64_t assignment_generation;
  int64_t user_id;
  bool is_audience;
  bool is_mute;
  bool camera_requested;
  bool camera_active;
  bool screen_requested;
  bool screen_active;
  char ufrag[32];
} sfu_membership_member_t;

typedef struct sfu_membership_recipient {
  sfu_peer_session_t *session;
  int fd;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint32_t mid_screen;
  uint32_t remote_slot;
  uint64_t assignment_generation;
  bool send_snapshot;
  bool send_delta;
  bool renegotiate;
} sfu_membership_recipient_t;

typedef struct sfu_membership_event {
  sfu_membership_event_kind_t kind;
  uint64_t room_id;
  uint64_t room_revision;
  uint32_t participant_count;
  uint32_t recipient_count;
  uint32_t member_count;
  uint32_t subject_peer_id;
  int64_t subject_user_id;
  bool subject_is_audience;
  bool subject_is_mute;
  bool subject_camera_requested;
  bool subject_camera_active;
  bool subject_screen_requested;
  bool subject_screen_active;
  bool preallocated_storage;
  sfu_peer_session_t *storage_owner;
  char subject_ufrag[32];
  sfu_membership_recipient_t recipients[SFU_ROOM_MAX_PEERS];
  sfu_membership_member_t members[SFU_ROOM_MAX_PEERS];
} sfu_membership_event_t;

typedef struct sfu_membership_reservation {
  struct sfu_signaling_server *server;
} sfu_membership_reservation_t;

typedef enum sfu_disconnect_reason {
  SFU_DISCONNECT_NORMAL = 1000,         /* Normal closure */
  SFU_DISCONNECT_GOING_AWAY = 1001,     /* Server shutting down */
  SFU_DISCONNECT_PROTOCOL_ERROR = 1002, /* Protocol error */
  SFU_DISCONNECT_POLICY_VIOLATION = 1008,
  SFU_DISCONNECT_INTERNAL_ERROR = 1011, /* Internal server error */

  /* Application-specific (4000+) */
  SFU_DISCONNECT_IDLE_TIMEOUT = 4001, /* Client idle too long */
  SFU_DISCONNECT_PING_FAILED = 4002,  /* Failed to send ping */
  SFU_DISCONNECT_AUTH_NOT_CONFIGURED = 4003,
  SFU_DISCONNECT_MISSING_TOKEN = 4004,
  SFU_DISCONNECT_INVALID_TOKEN = 4005,
  SFU_DISCONNECT_KICKED = 4006, /* Kicked by admin */
  SFU_DISCONNECT_WS_HANDSHAKE_FAILED = 4007,
  SFU_DISCONNECT_RECV_ERROR = 4008, /* WebSocket recv failure */
  SFU_DISCONNECT_POLL_START_FAILED = 4009,
  SFU_DISCONNECT_TRANSPORT_ERROR = 4010, /* UV_DISCONNECT / poll error */
} sfu_disconnect_reason_t;

typedef struct sfu_renegotiation_fallback_node {
  sfu_peer_session_t *session;
  struct sfu_renegotiation_fallback_node *next;
} sfu_renegotiation_fallback_node_t;

typedef struct sfu_renegotiation_queue {
  sfu_peer_session_t *items[SFU_RENEGOTIATION_QUEUE_CAP];
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  uint32_t fallback_count;
  sfu_renegotiation_fallback_node_t *fallback_head;
  sfu_renegotiation_fallback_node_t *fallback_tail;
  sfu_renegotiation_fallback_node_t emergency_fallback;
  bool emergency_fallback_used;
  pthread_mutex_t lock;
} sfu_renegotiation_queue_t;

_Static_assert(SFU_RENEGOTIATION_QUEUE_CAP >= SFU_SESSION_TABLE_MAX, "renegotiation queue must hold one pending reference per session");

typedef struct sfu_membership_queue {
  sfu_membership_event_t *items[SFU_MEMBERSHIP_QUEUE_CAP];
  sfu_peer_session_t *media_items[SFU_MEMBERSHIP_QUEUE_CAP];
  uint32_t head;
  uint32_t tail;
  uint32_t count;
  uint32_t reserved_count;
  uint32_t media_head;
  uint32_t media_tail;
  uint32_t media_count;
  bool accepting;
  pthread_mutex_t lock;
  pthread_cond_t not_full;
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
  bool suppress_wake;
  bool test_auto_drain;
  bool test_membership_only;
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
  uint8_t screen_codec_preference;
  bool disconnecting;
  bool keepalive_inited;
  bool in_registry;
  bool initial_answer_accepted;
} sfu_client_conn_t;

int sfu_signaling_server_start(sfu_signaling_server_t *s, uint16_t listen_port, const char *media_host, uint16_t media_port,
                               const sfu_ice_credentials_t *ice_creds, const sfu_dtls_ctx_t *dtls_ctx, sfu_session_table_t *sessions,
                               sfu_room_registry_t *room_registry, sfu_routing_table_t *routing_table);
void sfu_signaling_server_stop(sfu_signaling_server_t *s);
void sfu_signaling_trigger_peer_renegotiation(sfu_peer_session_t *session);
void sfu_signaling_schedule_pending_peer(sfu_peer_session_t *session);
bool sfu_signaling_reconcile_remote_slots(sfu_peer_session_t *session);
bool sfu_signaling_queue_membership_event(sfu_membership_event_t *event);
bool sfu_signaling_reserve_membership_event(sfu_membership_reservation_t *reservation);
void sfu_signaling_commit_membership_event(sfu_membership_reservation_t *reservation, sfu_membership_event_t *event);
void sfu_signaling_cancel_membership_event(sfu_membership_reservation_t *reservation);
void sfu_membership_event_release(sfu_membership_event_t *event);
sfu_membership_event_t *sfu_membership_event_alloc(void);
void sfu_membership_event_test_fail_allocations(uint32_t count);
void sfu_signaling_membership_test_server_init(sfu_signaling_server_t *s);
void sfu_signaling_membership_test_server_stop(sfu_signaling_server_t *s);
sfu_membership_event_t *sfu_signaling_membership_test_pop(sfu_signaling_server_t *s);
void sfu_signaling_renegotiation_test_server_init(sfu_signaling_server_t *s);
void sfu_signaling_renegotiation_test_server_stop(sfu_signaling_server_t *s);
sfu_peer_session_t *sfu_signaling_renegotiation_test_pop(sfu_signaling_server_t *s);
uint32_t sfu_signaling_renegotiation_test_count(sfu_signaling_server_t *s);
sfu_remote_offer_manifest_t *sfu_signaling_capture_offer_manifest(sfu_peer_session_t *session);
void sfu_signaling_notify_media_state(sfu_peer_session_t *peer);
void sfu_signaling_generate_turn_credentials(const char *secret, const char *username_suffix, char *out_username, size_t user_sz, char *out_password,
                                             size_t pass_sz, uint32_t ttl_seconds);
sfu_video_codec_t sfu_signaling_parse_screen_codec_preference(const char *json, size_t json_len);
uint32_t generate_unique_id(void);

#endif /* SFU_PROTOCOL_SIGNALING_H */
