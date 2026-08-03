#ifndef SFU_DATA_DEF_H
#define SFU_DATA_DEF_H

#include <openssl/ssl.h>
#include <srtp2/srtp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

// SRTP_AES128_CM_SHA1_80: 2 x (16-byte key + 14-byte salt)
#define SFU_SRTP_KEY_MATERIAL_LEN 60
// "XX:XX:...:XX\0" for SHA-256, 32 bytes -> 95 chars + nul
#define SFU_DTLS_FINGERPRINT_LEN 96

#define SFU_SESSION_TABLE_MAX 8192
#define SFU_ROOM_MAX_PEERS 256
#define SFU_MAX_REMOTE_SLOTS (SFU_ROOM_MAX_PEERS - 1)
#define SFU_MAX_REMOTE_TRANSCEIVERS (SFU_MAX_REMOTE_SLOTS * SFU_REMOTE_TRANSCEIVERS_PER_SLOT)
#define SFU_MAX_UPLINK_TRANSCEIVERS 3      /* audio, camera, screen */
#define SFU_REMOTE_TRANSCEIVERS_PER_SLOT 2 /* audio + video */

#define SFU_SIGNALING_RECV_CAP 16384
#define SFU_SIGNALING_SDP_CAP 16384
#define SFU_SIGNALING_JSON_CAP 32768

#define SFU_HASH_EMPTY UINT32_MAX
#define SFU_HASH_DELETED (UINT32_MAX - 1)

#define SFU_SESSION_ADDR_HASH_SLOTS 16384
#define SFU_SESSION_UFRAG_HASH_SLOTS 16384

#define SFU_MAX_PAYLOAD_SIZE 2048

typedef enum { SFU_MEDIA_AUDIO = 0, SFU_MEDIA_VIDEO, SFU_MEDIA_SCREEN, SFU_MEDIA_DATA } sfu_media_kind_t;

typedef enum { SFU_DIRECTION_INACTIVE = 0, SFU_DIRECTION_SENDONLY, SFU_DIRECTION_RECVONLY, SFU_DIRECTION_SENDRECV } sfu_direction_t;

typedef enum {
  SFU_SESSION_NEW = 0,
  SFU_SESSION_DTLS_HANDSHAKING,
  SFU_SESSION_ESTABLISHED,
  SFU_SESSION_FAILED,
} sfu_session_state_t;

/* Session lifecycle (Phase 3, F-01/F-03/F-04 fix):
 *  - OPEN: published in the session table, lookup-able, accepts work.
 *  - CLOSING: logically closed. Removed from the table and every hash index;
 *    no new references can be acquired. Existing refcounted pins keep the
 *    allocation (and its initialized DTLS/SRTP/RTX state) alive until the
 *    last sfu_session_release().
 * `active` stays true through CLOSING so that final teardown always destroys
 * initialized DTLS/SRTP state regardless of when logical close happened. */
typedef enum {
  SFU_SESSION_LIFECYCLE_OPEN = 0,
  SFU_SESSION_LIFECYCLE_CLOSING,
} sfu_session_lifecycle_t;

typedef enum {
  SFU_DTLS_FEED_ERROR = -1,
  SFU_DTLS_FEED_IN_PROGRESS = 0,
  SFU_DTLS_FEED_ESTABLISHED = 1,
} sfu_dtls_feed_status_t;

typedef struct sfu_peer_session sfu_peer_session_t;
typedef struct sfu_room sfu_room_t;
typedef struct gcc_bwe_context gcc_bwe_context_t;
typedef struct sfu_twcc_history sfu_twcc_history_t;
typedef struct sfu_subscriber_scheduler sfu_subscriber_scheduler_t;
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

/* One immutable routing entry inside a receiver snapshot. `subscriber` is a
 * retained (refcounted) destination session; it stays valid for as long as
 * the entry's snapshot is held, so a worker may use it for the full packet
 * path without any additional pinning. All remaining fields are value copies
 * of the publisher's routing metadata taken under the room lock; no mutable
 * transceiver chains are exposed to readers. */
typedef struct sfu_receiver_entry {
  sfu_peer_session_t *subscriber;
  char subscriber_ufrag[32];
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t video_rtx_ssrc;
  uint32_t mid_audio;
  uint32_t mid_video;
  uint8_t video_pt;
  uint8_t video_rtx_pt;
  bool has_audio;
  bool has_video;
  bool audio_active;
  bool video_active;
} sfu_receiver_entry_t;

/* Immutable, refcounted copy-on-write receiver set (F-03/F-04). Readers
 * acquire-load the session's snapshot pointer, retain it via a CAS on the
 * refcount (skipping snapshots already draining to zero), traverse entries,
 * and release. Writers build a fully-populated replacement under the room
 * lock and release-store it before dropping the old snapshot's writer
 * reference, so a live pointer can never reference a freed snapshot. The old
 * snapshot is freed once the last reader lets go. */
struct sfu_receiver_snapshot {
  _Atomic uint32_t refcount;
  uint64_t generation;
  uint32_t count;
  uint32_t capacity;
  sfu_receiver_entry_t entries[];
};

typedef struct {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  char ufrag[32];
  sfu_dtls_conn_t dtls;
} sfu_peer_session_cold_t;

typedef struct sfu_peer_session {
  sfu_room_t *room;
  gcc_bwe_context_t *gcc_ctx;
  sfu_twcc_history_t *twcc_history;
  sfu_subscriber_scheduler_t *scheduler;
  sfu_rtx_cache_t *rtx_cache;
  sfu_peer_session_cold_t *cold;
  _Atomic(sfu_receiver_snapshot_t *) receivers;
  sfu_srtp_ctx_t srtp;
  sfu_transceiver_t uplink_audio;
  sfu_transceiver_t uplink_video;
  sfu_transceiver_t screen;
  uint8_t pt_map[128];
  int64_t user_id;
  int64_t last_pli_time;
  int64_t last_fir_time;
  uint32_t peer_id;
  uint32_t next_remote_mid;
  int fd;
  uint16_t worker_id;
  _Atomic uint16_t next_twcc_seq;
  _Atomic uint32_t refcount;
  _Atomic uint8_t lifecycle;
  _Atomic bool accepts_work;
  uint8_t state;
  uint8_t target_sid;
  uint8_t target_tid;
  uint8_t fir_seq;
  bool active;
  bool negotiation_needed;
} sfu_peer_session_t;

typedef struct sfu_hash_slot {
  uint32_t hash;
  uint32_t index;
} sfu_hash_slot_t;

typedef struct sfu_session_table {
  sfu_peer_session_t **sessions;
  sfu_dtls_ctx_t *dtls_ctx;
  uint32_t capacity;
  uint32_t count;
  pthread_mutex_t lock;
  sfu_hash_slot_t addr_index[SFU_SESSION_ADDR_HASH_SLOTS];
  sfu_hash_slot_t ufrag_index[SFU_SESSION_UFRAG_HASH_SLOTS];
} sfu_session_table_t;

typedef struct sfu_room {
  sfu_peer_session_t **peers;
  uint64_t room_id;
  uint32_t peer_capacity;
  uint32_t peer_count;
  pthread_mutex_t lock;
} sfu_room_t;


#endif  // SFU_DATA_DEF_H
