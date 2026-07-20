#include "room/room.h"
#include "util/log.h"

#include <inttypes.h>
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

void sfu_room_set_publisher_ssrcs(sfu_room_t *room, const char *ufrag, uint32_t audio_ssrc, uint32_t video_ssrc, uint32_t rtx_ssrc) {
  pthread_mutex_lock(&room->lock);

  sfu_publisher_ssrc_t *slot = NULL;
  for (uint32_t i = 0; i < room->publisher_count; i++) {
    if (room->publishers[i].active && strcmp(room->publishers[i].ufrag, ufrag) == 0) {
      slot = &room->publishers[i];
      break;
    }
  }

  if (!slot) {
    if (room->publisher_count >= SFU_ROOM_MAX_PEERS) {
      SFU_LOG_WARN("room [%" PRIu64 "] publisher table full (%u), dropping ufrag=%s", room->room_id, SFU_ROOM_MAX_PEERS, ufrag);
      pthread_mutex_unlock(&room->lock);
      return;
    }
    slot = &room->publishers[room->publisher_count++];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->ufrag, ufrag, sizeof(slot->ufrag) - 1);
    slot->ufrag[sizeof(slot->ufrag) - 1] = '\0';
    slot->active = true;
  }

  /* Only overwrite fields this offer actually supplied SSRCs for. */
  if (audio_ssrc != 0) {
    slot->audio_ssrc = audio_ssrc;
  }
  if (video_ssrc != 0) {
    slot->video_ssrc = video_ssrc;
    slot->rtx_ssrc = rtx_ssrc;
  }

  pthread_mutex_unlock(&room->lock);
}

bool sfu_room_get_other_publisher_ssrcs(sfu_room_t *room, const char *self_ufrag, uint32_t *audio_ssrc, uint32_t *video_ssrc, uint32_t *rtx_ssrc) {
  *audio_ssrc = 0;
  *video_ssrc = 0;
  *rtx_ssrc = 0;

  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->publisher_count; i++) {
    sfu_publisher_ssrc_t *p = &room->publishers[i];
    if (p->active && strcmp(p->ufrag, self_ufrag) != 0) {
      *audio_ssrc = p->audio_ssrc;
      *video_ssrc = p->video_ssrc;
      *rtx_ssrc = p->rtx_ssrc;
      pthread_mutex_unlock(&room->lock);
      return true;
    }
  }
  pthread_mutex_unlock(&room->lock);
  return false;
}

void sfu_room_publish(sfu_room_t *room, const char *ufrag, int fd, const char *offer_sdp, size_t offer_sdp_len, uint32_t audio_ssrc, uint32_t video_ssrc,
                      uint32_t rtx_ssrc) {
  pthread_mutex_lock(&room->lock);

  sfu_publisher_ssrc_t *slot = NULL;
  for (uint32_t i = 0; i < room->publisher_count; i++) {
    if (room->publishers[i].active && strcmp(room->publishers[i].ufrag, ufrag) == 0) {
      slot = &room->publishers[i];
      break;
    }
  }
  if (!slot) {
    if (room->publisher_count >= SFU_ROOM_MAX_PEERS) {
      SFU_LOG_WARN("room [%" PRIu64 "] publisher table full (%u), dropping ufrag=%s", room->room_id, SFU_ROOM_MAX_PEERS, ufrag);
      pthread_mutex_unlock(&room->lock);
      return;
    }
    slot = &room->publishers[room->publisher_count++];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->ufrag, ufrag, sizeof(slot->ufrag) - 1);
    slot->active = true;
  }

  slot->fd = fd;
  slot->active = true;

  size_t copy_len = offer_sdp_len < sizeof(slot->offer_sdp) ? offer_sdp_len : sizeof(slot->offer_sdp) - 1;
  memcpy(slot->offer_sdp, offer_sdp, copy_len);
  slot->offer_sdp[copy_len] = '\0';
  slot->offer_sdp_len = copy_len;

  if (audio_ssrc != 0) {
    slot->audio_ssrc = audio_ssrc;
  }
  if (video_ssrc != 0) {
    slot->video_ssrc = video_ssrc;
    slot->rtx_ssrc = rtx_ssrc;
  }

  pthread_mutex_unlock(&room->lock);
}

void sfu_room_unpublish(sfu_room_t *room, const char *ufrag) {
  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->publisher_count; i++) {
    if (room->publishers[i].active && strcmp(room->publishers[i].ufrag, ufrag) == 0) {
      room->publishers[i].active = false;
      room->publishers[i].fd = -1;
      break;
    }
  }
  pthread_mutex_unlock(&room->lock);
}

uint32_t sfu_room_snapshot_other_publishers(sfu_room_t *room, const char *exclude_ufrag, sfu_publisher_snapshot_t *out, uint32_t max_out) {
  uint32_t n = 0;
  pthread_mutex_lock(&room->lock);
  for (uint32_t i = 0; i < room->publisher_count && n < max_out; i++) {
    sfu_publisher_ssrc_t *p = &room->publishers[i];
    if (!p->active || strcmp(p->ufrag, exclude_ufrag) == 0) {
      continue;
    }
    strncpy(out[n].ufrag, p->ufrag, sizeof(out[n].ufrag) - 1);
    out[n].ufrag[sizeof(out[n].ufrag) - 1] = '\0';
    out[n].fd = p->fd;
    memcpy(out[n].offer_sdp, p->offer_sdp, p->offer_sdp_len);
    out[n].offer_sdp_len = p->offer_sdp_len;
    n++;
  }
  pthread_mutex_unlock(&room->lock);
  return n;
}
