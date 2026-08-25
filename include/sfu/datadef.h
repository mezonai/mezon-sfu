#ifndef SFU_DATA_DEF_H
#define SFU_DATA_DEF_H

#include <openssl/ssl.h>
#include <srtp2/srtp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "rtp/rtp_seq_translate.h"

// Max DTLS-SRTP exporter length: GCM-256 is 2 x (32-byte key + 12-byte salt) = 88
#define SFU_SRTP_KEY_MATERIAL_LEN 88
_Static_assert(SFU_SRTP_KEY_MATERIAL_LEN >= 88, "GCM-256 DTLS-SRTP exporter writes 88 bytes");
// "XX:XX:...:XX\0" for SHA-256, 32 bytes -> 95 chars + nul
#define SFU_DTLS_FINGERPRINT_LEN 96

#define SFU_SESSION_TABLE_MAX 32768
#define SFU_ROOM_MAX_PEERS 300
#define SFU_MAX_REMOTE_SLOTS (SFU_ROOM_MAX_PEERS - 1)
#define SFU_MAX_REMOTE_TRANSCEIVERS (SFU_MAX_REMOTE_SLOTS * SFU_REMOTE_TRANSCEIVERS_PER_SLOT)
#define SFU_MAX_UPLINK_TRANSCEIVERS 3      /* audio, camera, screen */
#define SFU_REMOTE_TRANSCEIVERS_PER_SLOT 3 /* audio + camera + screen */
#define SFU_LOCAL_AUDIO_MID 0u
#define SFU_LOCAL_CAMERA_MID 1u
#define SFU_LOCAL_SCREEN_MID 2u
#define SFU_REMOTE_MID_BASE 3u

#define SFU_SIGNALING_RECV_CAP 1048576
#define SFU_SIGNALING_SDP_CAP 1048576
#define SFU_SIGNALING_JSON_CAP 2097152

#define SFU_HASH_EMPTY UINT32_MAX
#define SFU_HASH_DELETED (UINT32_MAX - 1)

#define SFU_SESSION_ADDR_HASH_SLOTS 65536
#define SFU_SESSION_UFRAG_HASH_SLOTS 65536

#define SFU_MAX_PAYLOAD_SIZE 2048

#define SFU_PT_VP9 98
#define SFU_PT_VP9_RTX 99
#define SFU_PT_AV1 100
#define SFU_PT_AV1_RTX 101
#define SFU_PT_VP8 96
#define SFU_PT_VP8_RTX 97
#define SFU_PT_H264 102
#define SFU_PT_H264_RTX 103

typedef enum { SFU_MEDIA_AUDIO = 0, SFU_MEDIA_VIDEO, SFU_MEDIA_SCREEN, SFU_MEDIA_DATA } sfu_media_kind_t;

typedef enum { SFU_DIRECTION_INACTIVE = 0, SFU_DIRECTION_SENDONLY, SFU_DIRECTION_RECVONLY, SFU_DIRECTION_SENDRECV } sfu_direction_t;

typedef enum {
  SFU_SESSION_NEW = 0,
  SFU_SESSION_DTLS_HANDSHAKING,
  SFU_SESSION_ESTABLISHED,
  SFU_SESSION_FAILED,
} sfu_session_state_t;

typedef enum {
  SFU_SESSION_LIFECYCLE_OPEN = 0,
  SFU_SESSION_LIFECYCLE_CLOSING,
} sfu_session_lifecycle_t;

typedef enum {
  SFU_DTLS_FEED_ERROR = -1,
  SFU_DTLS_FEED_IN_PROGRESS = 0,
  SFU_DTLS_FEED_ESTABLISHED = 1,
} sfu_dtls_feed_status_t;

typedef enum {
  SFU_VIDEO_CODEC_NONE = 0,
  SFU_VIDEO_CODEC_VP8,
  SFU_VIDEO_CODEC_VP9,
  SFU_VIDEO_CODEC_AV1,
  SFU_VIDEO_CODEC_H264,
} sfu_video_codec_t;


typedef enum {
  SFU_PACER_CLASS_AUDIO = 0,
  SFU_PACER_CLASS_RTX,
  SFU_PACER_CLASS_VIDEO_BASE,
  SFU_PACER_CLASS_VIDEO_TRANSITION,
  SFU_PACER_CLASS_VIDEO_ENH,
  SFU_PACER_CLASS_COUNT
} sfu_pacer_class_t;

typedef struct sfu_peer_session sfu_peer_session_t;
typedef struct sfu_room sfu_room_t;
typedef struct sfu_membership_event sfu_membership_event_t;
typedef struct gcc_bwe_context gcc_bwe_context_t;
typedef struct sfu_twcc_history sfu_twcc_history_t;
typedef struct sfu_twcc_recv_tracker sfu_twcc_recv_tracker_t;
typedef struct sfu_layer_scheduler_slot sfu_layer_scheduler_slot_t;
typedef struct sfu_rtx_cache sfu_rtx_cache_t;
typedef struct sfu_receiver_snapshot sfu_receiver_snapshot_t;

typedef struct sfu_srtp_ctx {
  srtp_t inbound;
  srtp_t outbound;
} sfu_srtp_ctx_t;

typedef struct sfu_dtls_ctx {
  SSL_CTX *ssl_ctx;
  char fingerprint[SFU_DTLS_FINGERPRINT_LEN];
} sfu_dtls_ctx_t;

typedef struct {
  SSL *ssl;
  BIO *rbio;
  BIO *wbio;
  unsigned long srtp_profile_id;
  uint8_t srtp_keying_material[SFU_SRTP_KEY_MATERIAL_LEN];
  bool established;
} sfu_dtls_conn_t;

typedef struct {
  char stream_id[64];
  char track_id[64];
  char cname[64];
} sfu_transceiver_metadata_t;

typedef struct sfu_transceiver {
  sfu_peer_session_t *owner;
  sfu_video_codec_t codec;
  uint32_t ssrc;
  uint32_t rtx_ssrc;
  uint16_t mid;
  uint8_t payload_type;
  uint8_t rtx_payload_type;
  uint8_t kind;
  uint8_t direction;
  bool active;
  sfu_transceiver_metadata_t *metadata;
} sfu_transceiver_t;

typedef struct sfu_receiver_entry {
  sfu_peer_session_t *subscriber;
  char subscriber_ufrag[32];
  int64_t publisher_user_id;
  uint32_t publisher_peer_id;
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint32_t screen_ssrc;
  uint32_t screen_rtx_ssrc;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint32_t mid_screen;
  uint32_t remote_slot;
  uint64_t assignment_generation;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  uint8_t screen_pt;
  uint8_t screen_rtx_pt;
  sfu_video_codec_t video_codec;
  sfu_video_codec_t screen_codec;
  bool has_audio;
  bool has_video;
  bool has_screen;
  bool audio_active;
  bool video_active;
  bool screen_active;
} sfu_receiver_entry_t;

#define SFU_RECEIVER_CHUNK_SHIFT 5u
#define SFU_RECEIVER_CHUNK_SIZE (1u << SFU_RECEIVER_CHUNK_SHIFT)
#define SFU_RECEIVER_CHUNK_MASK (SFU_RECEIVER_CHUNK_SIZE - 1u)
#define SFU_RECEIVER_CHUNK_COUNT ((SFU_MAX_REMOTE_SLOTS + SFU_RECEIVER_CHUNK_MASK) / SFU_RECEIVER_CHUNK_SIZE)

typedef struct sfu_receiver_chunk {
  _Atomic uint32_t refcount;
  uint32_t occupied;
  sfu_receiver_entry_t entries[SFU_RECEIVER_CHUNK_SIZE];
} sfu_receiver_chunk_t;

struct sfu_receiver_snapshot {
  _Atomic uint32_t refcount;
  uint64_t generation;
  uint32_t count;
  sfu_receiver_chunk_t *chunks[SFU_RECEIVER_CHUNK_COUNT];
};

#define SFU_FANOUT_CHUNK_SHIFT 5u
#define SFU_FANOUT_CHUNK_SIZE (1u << SFU_FANOUT_CHUNK_SHIFT)
#define SFU_FANOUT_CHUNK_MASK (SFU_FANOUT_CHUNK_SIZE - 1u)
#define SFU_FANOUT_CHUNK_COUNT ((SFU_MAX_REMOTE_SLOTS + SFU_FANOUT_CHUNK_MASK) / SFU_FANOUT_CHUNK_SIZE)

#define SFU_FANOUT_AUDIO 0x1u
#define SFU_FANOUT_VIDEO 0x2u
#define SFU_FANOUT_SCREEN 0x4u

typedef struct sfu_fanout_route {
  sfu_peer_session_t *subscriber;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint32_t screen_ssrc;
  uint32_t screen_rtx_ssrc;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint32_t mid_screen;
  uint32_t remote_slot;
  uint64_t assignment_generation;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  uint8_t screen_pt;
  uint8_t screen_rtx_pt;
} sfu_fanout_route_t;

typedef struct sfu_fanout_chunk {
  _Atomic uint32_t refcount;
  uint32_t occupied;
  uint32_t audio_eligible;
  uint32_t video_eligible;
  uint32_t screen_eligible;
  sfu_fanout_route_t routes[SFU_FANOUT_CHUNK_SIZE];
} sfu_fanout_chunk_t;

typedef struct sfu_fanout_bundle {
  _Atomic uint32_t refcount;
  uint64_t generation;
  uint32_t count;
  sfu_fanout_chunk_t *chunks[SFU_FANOUT_CHUNK_COUNT];
} sfu_fanout_bundle_t;

typedef struct sfu_media_snapshot {
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint32_t screen_ssrc;
  uint32_t screen_rtx_ssrc;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  uint8_t screen_pt;
  uint8_t screen_rtx_pt;
  sfu_video_codec_t video_codec;
  sfu_video_codec_t screen_codec;
  uint8_t twcc_recv_extmap_id;
  uint8_t twcc_send_extmap_id;
  uint8_t mid_recv_extmap_id;
  bool audio_active;
  bool video_active;
  bool screen_active;
} sfu_media_snapshot_t;

typedef enum {
  SFU_VIDEO_RUNTIME_UNINITIALIZED = 0,
  SFU_VIDEO_RUNTIME_INITIALIZING,
  SFU_VIDEO_RUNTIME_READY,
  SFU_VIDEO_RUNTIME_FAILED,
} sfu_video_runtime_state_t;

typedef struct sfu_pacer {
  uint64_t sent[SFU_PACER_CLASS_COUNT];
  int64_t balance_bytes;
  int64_t bucket_cap_bytes;
  int64_t last_refill_us;
  uint64_t dropped_enh;
  uint64_t rtx_dropped_budget;
  int64_t rtx_budget_bytes;
  int64_t rtx_budget_cap_bytes;
  int64_t rtx_last_refill_us;
  uint32_t pacing_bps;
  uint32_t rtx_budget_bps;
  bool active;
} sfu_pacer_t;

typedef struct {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  char ufrag[32];
  sfu_dtls_conn_t dtls;
  sfu_dtls_conn_t pending_dtls;
  uint8_t active_client_random[32];
  uint8_t pending_client_random[32];
  uint64_t pending_dtls_started_ms;
  uint64_t last_srtp_failure_log_ms;
  _Atomic uint32_t transport_generation;
  uint32_t suppressed_srtp_failures;
  int last_srtp_failure_status;
  bool active_client_random_valid;
  bool pending_dtls_active;
  sfu_rtp_seq_translator_t rtp_seq_translator;
  void *table;
  uint32_t table_index;
} sfu_peer_session_cold_t;

typedef struct {
  pthread_mutex_t lock;
  bool negotiation_needed;
  bool renegotiation_queued;
  bool offer_outstanding;
  bool renegotiation_pending;
  uint64_t offer_generation;
  uint32_t negotiation_retry_count;
  uint64_t desired_offer_revision;
  uint64_t offered_revision;
  uint64_t answered_revision;
  uint64_t last_answered_offer_generation;
  uint64_t negotiation_first_dirty_ms;
  uint64_t negotiation_due_ms;
} sfu_session_negotiation_t;

typedef enum {
  SFU_REMOTE_SLOT_FREE = 0,
  SFU_REMOTE_SLOT_ACTIVE,
  SFU_REMOTE_SLOT_RETIRING,
  SFU_REMOTE_SLOT_RETIRING_OFFERED,
} sfu_remote_slot_state_t;

typedef struct {
  uint64_t assignment_generation;
  int64_t publisher_user_id;
  uint32_t publisher_peer_id;
  uint8_t state;
} sfu_remote_slot_t;

typedef struct sfu_remote_offer_manifest {
  _Atomic uint32_t refcount;
  uint64_t offer_generation;
  uint32_t high_water_slots;
  sfu_receiver_snapshot_t *receiver_root;
  uint64_t assignment_generations[SFU_MAX_REMOTE_SLOTS];
} sfu_remote_offer_manifest_t;

_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "remote slot authorization requires lock-free uint64 atomics");

typedef struct {
  sfu_remote_slot_t slots[SFU_MAX_REMOTE_SLOTS];
  _Atomic uint64_t applied_assignment_generations[SFU_MAX_REMOTE_SLOTS];
  sfu_remote_offer_manifest_t *offered_manifest;
  uint64_t next_assignment_generation;
  uint64_t next_offer_generation;
  uint32_t high_water_slots;
  uint32_t offered_slot_floor;
} sfu_remote_slot_table_t;

typedef struct {
  _Atomic uint32_t sequence;
  _Atomic uint64_t assignment_generation;
  _Atomic uint64_t updated_at_us;
  _Atomic uint32_t camera_bitrate_bps;
  _Atomic uint32_t screen_bitrate_bps;
} sfu_remb_contribution_t;

typedef struct {
  pthread_mutex_t lock;
  _Atomic(sfu_receiver_snapshot_t *) receivers;
  _Atomic(sfu_fanout_bundle_t *) fanout_bundle;
  sfu_remote_slot_table_t remote_slots;
  sfu_remb_contribution_t remb_contributions[SFU_MAX_REMOTE_SLOTS];
} sfu_session_graph_t;

typedef struct {
  pthread_mutex_t lock;
  sfu_transceiver_t uplink_audio;
  sfu_transceiver_t uplink_video;
  sfu_transceiver_t screen;
  _Atomic uint64_t snapshot_words[5];
  _Atomic uint32_t snapshot_seq;
  uint8_t pt_map[128];
  uint8_t twcc_recv_extmap_id;
  uint8_t twcc_send_extmap_id;
  uint8_t mid_recv_extmap_id;
  _Atomic bool ptt_active;
  _Atomic bool camera_enabled;
  _Atomic bool screen_enabled;
  _Atomic bool camera_rtp_observed;
  _Atomic bool screen_rtp_observed;
  _Atomic bool media_update_queued;
  _Atomic bool camera_announced_active;
  _Atomic bool screen_announced_active;
  _Atomic bool audio_send_negotiated;
  _Atomic bool video_send_negotiated;
  _Atomic bool screen_send_negotiated;
  _Atomic bool visible;
  _Atomic bool is_mute;
  _Atomic bool uplink_ssrc_dirty;
} sfu_session_media_t;

typedef struct sfu_congestion_diag {
  uint64_t last_log_us;
  uint64_t last_twcc_us;
  uint64_t last_allocation_us;
  uint64_t nack_requests;
  uint64_t cache_hits;
  uint64_t cache_misses;
  uint64_t rtx_sent;
  uint64_t pli_received;
  uint64_t pli_sent;
  uint64_t pli_coalesced;
  uint64_t last_logged_nack_requests;
  uint64_t last_logged_cache_hits;
  uint64_t last_logged_cache_misses;
  uint64_t last_logged_rtx_sent;
  uint64_t last_logged_pli_received;
  uint64_t last_logged_pli_sent;
  uint64_t last_logged_pli_coalesced;
  uint64_t last_logged_pacer_drops;
  uint64_t last_logged_rtx_budget_drops;
  uint32_t latest_gcc_bps;
  uint32_t latest_ack_bps;
  uint32_t latest_twcc_lost;
  uint32_t latest_twcc_total;
  uint32_t allocation_streams;
  uint32_t allocation_pool_bps;
  uint32_t allocation_reserve_bps;
  uint32_t allocation_allocated_bps;
  uint32_t allocation_unallocated_bps;
  uint32_t remb_contribution_bps;
  uint32_t remb_target_bps;
  uint32_t remb_fresh;
  uint32_t remb_stale;
  uint8_t latest_overuse;
  bool remb_sent;
} sfu_congestion_diag_t;

typedef struct {
  gcc_bwe_context_t *gcc_ctx;
  sfu_twcc_history_t *twcc_history;
  sfu_twcc_recv_tracker_t *twcc_recv;
  sfu_layer_scheduler_slot_t *schedulers;
  sfu_pacer_t pacer;
  sfu_rtx_cache_t *rtx_cache;
  sfu_congestion_diag_t diag;
  int64_t last_pli_time;
  int64_t last_screen_pli_time;
  int64_t last_fir_time;
  int64_t twcc_last_feedback_ref_us;
  int64_t last_camera_remb_time_us;
  int64_t last_screen_remb_time_us;
  uint32_t last_camera_remb_bps;
  uint32_t last_screen_remb_bps;
  _Atomic uint32_t generation;
  _Atomic uint16_t next_twcc_seq;
  _Atomic uint8_t video_runtime_state;
  uint8_t fir_seq;
} sfu_session_egress_t;

typedef struct sfu_peer_session {
  sfu_room_t *room;
  uint32_t room_slot;
  sfu_peer_session_cold_t *cold;
  sfu_session_negotiation_t negotiation;
  sfu_session_graph_t graph;
  sfu_session_media_t media;
  sfu_session_egress_t egress;
  sfu_srtp_ctx_t srtp;
  sfu_srtp_ctx_t previous_srtp;
  uint64_t previous_srtp_expires_ms;
  pthread_mutex_t crypto_lock;
  pthread_mutex_t answer_lock;
  pthread_mutex_t ingress_lock;
  pthread_mutex_t membership_lock;
  sfu_membership_event_t *leave_event;
  _Atomic bool leave_event_in_use;
  int64_t user_id;
  uint32_t peer_id;
  _Atomic uint32_t applied_answer_generation;
  _Atomic uint64_t worker_owner;
  _Atomic uint32_t refcount;
  _Atomic uint8_t lifecycle;
  _Atomic bool accepts_work;
  _Atomic bool is_audience;
  int fd;
  uint8_t state;
  bool active;
} sfu_peer_session_t;

typedef struct sfu_hash_slot {
  uint32_t hash;
  uint32_t index;
} sfu_hash_slot_t;

typedef struct sfu_epoch_reclaimer sfu_epoch_reclaimer_t;

typedef struct sfu_session_table {
  sfu_peer_session_t **sessions;
  sfu_dtls_ctx_t *dtls_ctx;
  sfu_epoch_reclaimer_t *reclaimer;
  uint32_t *free_indices;
  uint32_t capacity;
  uint32_t count;
  uint32_t active_count;
  uint32_t free_count;
  pthread_rwlock_t lock;
  pthread_mutex_t ice_lock;
  sfu_hash_slot_t addr_index[SFU_SESSION_ADDR_HASH_SLOTS];
  sfu_hash_slot_t ufrag_index[SFU_SESSION_UFRAG_HASH_SLOTS];
} sfu_session_table_t;

typedef struct sfu_room {
  sfu_peer_session_t *peers[SFU_ROOM_MAX_PEERS];
  uint16_t free_slots[SFU_ROOM_MAX_PEERS];
  uint8_t occupied[SFU_ROOM_MAX_PEERS];
  uint64_t room_id;
  uint32_t peer_capacity;
  uint32_t peer_count;
  uint32_t free_count;
  uint64_t membership_revision;
  pthread_mutex_t lock;
} sfu_room_t;

#endif  // SFU_DATA_DEF_H
