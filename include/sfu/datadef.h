#ifndef SFU_DATA_DEF_H
#define SFU_DATA_DEF_H

#include <openssl/ssl.h>
#include <srtp2/srtp.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#define SFU_SRTP_KEY_MATERIAL_LEN 60 /* SRTP_AES128_CM_SHA1_80: 2 x (16-byte key + 14-byte salt) */
#define SFU_DTLS_FINGERPRINT_LEN 96  /* "XX:XX:...:XX\0" for SHA-256, 32 bytes -> 95 chars + nul */
#define SFU_SESSION_TABLE_MAX 8192
#define SFU_ROOM_MAX_PEERS 256
#define SFU_MAX_REMOTE_SLOTS (SFU_ROOM_MAX_PEERS - 1)

#define SFU_SIGNALING_RECV_CAP 16384
#define SFU_SIGNALING_SDP_CAP 16384
#define SFU_SIGNALING_JSON_CAP 32768

#define SFU_HASH_EMPTY UINT32_MAX
#define SFU_HASH_DELETED (UINT32_MAX - 1)

#define SFU_SESSION_ADDR_HASH_SLOTS 16384
#define SFU_SESSION_UFRAG_HASH_SLOTS 16384

typedef enum { SFU_MEDIA_AUDIO = 0, SFU_MEDIA_VIDEO, SFU_MEDIA_SCREEN, SFU_MEDIA_DATA } sfu_media_kind_t;

typedef enum { SFU_DIRECTION_INACTIVE = 0, SFU_DIRECTION_SENDONLY, SFU_DIRECTION_RECVONLY, SFU_DIRECTION_SENDRECV } sfu_direction_t;

typedef enum {
  SFU_SESSION_NEW = 0,
  SFU_SESSION_DTLS_HANDSHAKING,
  SFU_SESSION_ESTABLISHED,
  SFU_SESSION_FAILED,
} sfu_session_state_t;

typedef enum {
  SFU_DTLS_FEED_ERROR = -1,      /* fatal: drop this connection/session */
  SFU_DTLS_FEED_IN_PROGRESS = 0, /* handshake continuing; drain output and send it */
  SFU_DTLS_FEED_ESTABLISHED = 1, /* handshake complete; srtp_keying_material is valid */
} sfu_dtls_feed_status_t;

struct sfu_peer_session;
struct sfu_room;

typedef struct sfu_srtp_ctx {
  srtp_t inbound;  /* decrypts packets FROM this peer */
  srtp_t outbound; /* encrypts packets TO this peer   */
} sfu_srtp_ctx_t;

typedef struct sfu_dtls_ctx {
  SSL_CTX *ssl_ctx;
  char fingerprint[SFU_DTLS_FINGERPRINT_LEN]; /* certificate SHA-256, colon-hex */
} sfu_dtls_ctx_t;

typedef struct sfu_dtls_conn {
  SSL *ssl;
  BIO *rbio; /* received datagrams get written here before SSL_do_handshake */
  BIO *wbio; /* OpenSSL writes its desired output here for us to drain+send */
  bool established;
  unsigned long srtp_profile_id;
  uint8_t srtp_keying_material[SFU_SRTP_KEY_MATERIAL_LEN];
} sfu_dtls_conn_t;

typedef struct sfu_transceiver {
  uint16_t mid;
  sfu_media_kind_t kind;
  sfu_direction_t direction;
  bool active;
  uint32_t ssrc;
  uint32_t rtx_ssrc;
  uint8_t payload_type;
  uint8_t rtx_payload_type;
  char stream_id[64];
  char track_id[64];
  char cname[64];
  struct sfu_peer_session *owner;
} sfu_transceiver_t;

typedef struct sfu_receiver_slot {
  sfu_transceiver_t *video;
  sfu_transceiver_t *audio;
  uint32_t mid_audio;
  uint32_t mid_video;
} sfu_receiver_slot_t;

typedef struct sfu_peer_session {
  int fd;
  struct sockaddr_storage addr;
  socklen_t addr_len;
  uint16_t worker_id;
  sfu_session_state_t state;
  sfu_dtls_conn_t dtls;
  sfu_srtp_ctx_t srtp;
  struct sfu_room *room;
  bool active;
  char ufrag[32];
  uint8_t pt_map[128];
  sfu_transceiver_t uplink_audio;
  sfu_transceiver_t uplink_video;
  sfu_transceiver_t screen;
  sfu_receiver_slot_t **receivers;
  uint32_t receiver_capacity;
  bool negotiation_needed;
  uint32_t next_remote_mid;
} sfu_peer_session_t;

typedef struct sfu_hash_slot {
  uint32_t hash;
  uint32_t index;
} sfu_hash_slot_t;

typedef struct sfu_session_table {
  sfu_peer_session_t **sessions;
  uint32_t capacity;
  uint32_t count;
  pthread_mutex_t lock;
  sfu_dtls_ctx_t *dtls_ctx;
  sfu_hash_slot_t addr_index[SFU_SESSION_ADDR_HASH_SLOTS];
  sfu_hash_slot_t ufrag_index[SFU_SESSION_UFRAG_HASH_SLOTS];
} sfu_session_table_t;

typedef struct sfu_room {
  uint64_t room_id;
  sfu_peer_session_t **peers;
  uint32_t peer_capacity;
  uint32_t peer_count;
  pthread_mutex_t lock;
} sfu_room_t;


#endif  // SFU_DATA_DEF_H
