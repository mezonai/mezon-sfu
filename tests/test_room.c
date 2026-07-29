#include "room/room.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t receiver_count(sfu_peer_session_t *peer) {
  uint32_t n = 0;

  for (uint32_t i = 0; i < peer->receiver_capacity; i++) {
    if (peer->receivers[i]->audio || peer->receivers[i]->video) {
      n++;
    }
  }

  return n;
}

int main(void) {
  sfu_room_t room;

  assert(sfu_room_init(&room, 1, "test_room") == 0);

  sfu_peer_session_t a = {0};
  sfu_peer_session_t b = {0};
  sfu_peer_session_t c = {0};

  a.active = true;
  b.active = true;
  c.active = true;

  strcpy(a.ufrag, "a");
  strcpy(b.ufrag, "b");
  strcpy(c.ufrag, "c");

  a.uplink_audio.active = true;
  a.uplink_video.active = true;

  b.uplink_audio.active = true;
  b.uplink_video.active = true;

  c.uplink_audio.active = true;
  c.uplink_video.active = true;

  room_add_peer(&room, &a);

  assert(receiver_count(&a) == 0);

  room_add_peer(&room, &b);

  assert(receiver_count(&a) == 1);
  assert(receiver_count(&b) == 1);

  room_add_peer(&room, &c);

  assert(receiver_count(&a) == 2);
  assert(receiver_count(&b) == 2);
  assert(receiver_count(&c) == 2);

  /* Verify A subscribes to B and C */
  bool saw_b = false;
  bool saw_c = false;

  for (uint32_t i = 0; i < a.receiver_capacity; i++) {
    sfu_receiver_slot_t *slot = a.receivers[i];

    if (!slot->video) {
      continue;
    }

    if (slot->video == &b.uplink_video) {
      saw_b = true;
    }

    if (slot->video == &c.uplink_video) {
      saw_c = true;
    }
  }

  assert(saw_b);
  assert(saw_c);

  sfu_room_destroy(&room);

  printf("test_room: OK\n");
  return 0;
}
