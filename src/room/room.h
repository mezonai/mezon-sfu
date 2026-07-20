#ifndef SFU_ROOM_ROOM_H
#define SFU_ROOM_ROOM_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "sfu/config.h"

/*
 * Minimal peer registry standing in for real room/publish/subscribe
 * signaling.
 */
typedef struct sfu_peer_entry {
  struct sockaddr_storage addr;
  socklen_t addr_len;
  uint32_t worker_id; /* which worker core owns this peer's send path */
  bool active;
} sfu_peer_entry_t;

typedef struct sfu_room {
  uint32_t audio_ssrc;
  uint32_t video_ssrc;
  uint32_t rtx_ssrc;
  uint64_t room_id;    /* Unique numeric room identifier */
  char room_name[128]; /* User-friendly room name */
  sfu_peer_entry_t peers[SFU_ROOM_MAX_PEERS];
  uint32_t peer_count;
  pthread_mutex_t lock;
} sfu_room_t;

/* Updated initialization signature to accept uint64_t for room_id */
int sfu_room_init(sfu_room_t *room, uint64_t room_id, const char *room_name);
void sfu_room_destroy(sfu_room_t *room);

/* Registers a peer's address + owning worker on first sight */
void sfu_room_touch_peer(sfu_room_t *room, const struct sockaddr_storage *addr, socklen_t addr_len, uint32_t worker_id);

/* Copies up to max_out peer entries, excluding the peer matching 'exclude' */
uint32_t sfu_room_list_subscribers_excluding(sfu_room_t *room, const struct sockaddr_storage *exclude, socklen_t exclude_len, sfu_peer_entry_t *out,
                                             uint32_t max_out);

#endif /* SFU_ROOM_ROOM_H */
