#include "room/room.h"
#include "util/log.h"

#include <inttypes.h> /* Required for PRIu64 */
#include <string.h>

int sfu_room_init(sfu_room_t *room, uint64_t room_id, const char *room_name) {
  memset(room, 0, sizeof(*room));

  room->room_id = room_id;

  if (room_name) {
    strncpy(room->room_name, room_name, sizeof(room->room_name) - 1);
    room->room_name[sizeof(room->room_name) - 1] = '\0';
  }

  if (pthread_mutex_init(&room->lock, NULL) != 0) {
    return -1;
  }
  return 0;
}

void sfu_room_destroy(sfu_room_t *room) { pthread_mutex_destroy(&room->lock); }

static bool addr_equal(const struct sockaddr_storage *a, socklen_t a_len, const struct sockaddr_storage *b, socklen_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  return memcmp(a, b, a_len) == 0;
}

void sfu_room_touch_peer(sfu_room_t *room, const struct sockaddr_storage *addr, socklen_t addr_len, uint32_t worker_id) {
  pthread_mutex_lock(&room->lock);

  for (uint32_t i = 0; i < room->peer_count; i++) {
    if (room->peers[i].active && addr_equal(&room->peers[i].addr, room->peers[i].addr_len, addr, addr_len)) {
      room->peers[i].worker_id = worker_id; /* refresh in case it moved */
      pthread_mutex_unlock(&room->lock);
      return;
    }
  }

  if (room->peer_count < SFU_ROOM_MAX_PEERS) {
    sfu_peer_entry_t *e = &room->peers[room->peer_count++];
    memcpy(&e->addr, addr, addr_len);
    e->addr_len = addr_len;
    e->worker_id = worker_id;
    e->active = true;
  } else {
    SFU_LOG_WARN("room [%" PRIu64 "] peer table full (%u), dropping new peer", room->room_id, SFU_ROOM_MAX_PEERS);
  }

  pthread_mutex_unlock(&room->lock);
}

uint32_t sfu_room_list_subscribers_excluding(sfu_room_t *room, const struct sockaddr_storage *exclude, socklen_t exclude_len, sfu_peer_entry_t *out,
                                             uint32_t max_out) {
  uint32_t n = 0;

  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->peer_count && n < max_out; i++) {
    if (!room->peers[i].active) {
      continue;
    }
    if (addr_equal(&room->peers[i].addr, room->peers[i].addr_len, exclude, exclude_len)) {
      continue; /* don't echo a publisher's own packet back to it */
    }
    out[n++] = room->peers[i];
  }
  pthread_mutex_unlock(&room->lock);

  return n;
}
