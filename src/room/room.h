#ifndef SFU_ROOM_ROOM_H
#define SFU_ROOM_ROOM_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include "media/graph.h"
#include "sfu/config.h"

typedef struct sfu_peer_session sfu_peer_session_t;

typedef struct sfu_publisher_ssrc {
  int fd;
  char ufrag[32];
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t rtx_ssrc;
  char *offer_sdp;
  size_t offer_sdp_len;
  size_t offer_sdp_cap;
  bool active;
} sfu_publisher_ssrc_t;

typedef struct sfu_publisher_snapshot {
  int fd;
  char ufrag[32];
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t rtx_ssrc;
  char *offer_sdp;
  size_t offer_sdp_len;
} sfu_publisher_snapshot_t;

typedef struct sfu_room {
  uint64_t room_id;
  char room_name[32];
  sfu_peer_session_t *peers[SFU_ROOM_MAX_PEERS];
  uint32_t peer_count;
  sfu_publisher_ssrc_t publishers[SFU_ROOM_MAX_PEERS];
  uint32_t publisher_count;
  pthread_mutex_t lock;
  sfu_media_graph_t graph;
} sfu_room_t;

/* Updated initialization signature to accept uint64_t for room_id */
int sfu_room_init(sfu_room_t *room, uint64_t room_id, const char *room_name);
void sfu_room_destroy(sfu_room_t *room);

/* Records/updates the SSRCs a given client (identified by ice-ufrag) is
 * publishing. Only fields with a nonzero value overwrite the stored ones,
 * so an offer that doesn't touch a track won't clobber it. */
void sfu_room_set_publisher_ssrcs(sfu_room_t *room, const char *ufrag, uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc);

/* Finds the SSRCs of a publisher OTHER than `self_ufrag`, for building that
 * peer's answer. Returns true and fills the out-params if a different
 * active publisher exists, false otherwise (e.g. first peer in an empty
 * room, or self is the only publisher so far). NOTE: with more than two
 * participants this only returns ONE other publisher's SSRCs; a real
 * N-way room needs per-subscriber answers built per remote track, not a
 * single triple. */
bool sfu_room_get_other_publisher_ssrcs(sfu_room_t *room, const char *self_ufrag, uint32_t *audio_ssrc, uint32_t *video_ssrc, uint32_t *rtx_ssrc);

/* Upserts a peer's cached offer/fd and (if nonzero) their published SSRCs.
 * Called on every offer from a peer, whether or not it changes their SSRCs,
 * so their fd+offer are always available as a push target. */
void sfu_room_publish(sfu_room_t *room, const char *ufrag, int fd, const char *offer_sdp, size_t offer_sdp_len, uint32_t audio_ssrc, uint32_t video_ssrc,
                      uint32_t rtx_ssrc);

/* Marks a peer's entry inactive on disconnect. MUST be called when their
 * signaling connection closes, or a later push could write to a stale/
 * reused fd belonging to a different connection. */
void sfu_room_unpublish(sfu_room_t *room, const char *ufrag);

#endif /* SFU_ROOM_ROOM_H */
