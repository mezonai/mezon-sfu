#include "room/room.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct sockaddr_storage make_addr(uint16_t port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  struct sockaddr_storage s;
  memset(&s, 0, sizeof(s));
  memcpy(&s, &a, sizeof(a));
  return s;
}

int main(void) {
  sfu_room_t room;
  assert(sfu_room_init(&room, 0, DEFAULT_ROOM_NAME) == 0);

  struct sockaddr_storage a = make_addr(1000);
  struct sockaddr_storage b = make_addr(2000);
  struct sockaddr_storage c = make_addr(3000);
  socklen_t len = sizeof(struct sockaddr_in);

  sfu_room_touch_peer(&room, &a, len, /*worker_id=*/0);
  sfu_room_touch_peer(&room, &b, len, /*worker_id=*/1);
  sfu_room_touch_peer(&room, &c, len, /*worker_id=*/1);

  /* Subscribers for a's packet: b and c, not a itself. */
  sfu_peer_entry_t out[SFU_ROOM_MAX_PEERS];
  uint32_t n = sfu_room_list_subscribers_excluding(&room, &a, len, out, SFU_ROOM_MAX_PEERS);
  assert(n == 2);
  for (uint32_t i = 0; i < n; i++) {
    assert(memcmp(&out[i].addr, &a, len) != 0);
  }

  /* Re-touching an existing peer refreshes worker_id rather than
   * duplicating the entry. */
  sfu_room_touch_peer(&room, &a, len, /*worker_id=*/2);
  n = sfu_room_list_subscribers_excluding(&room, &b, len, out, SFU_ROOM_MAX_PEERS);
  assert(n == 2); /* still a and c, no duplicate */

  bool found_a_with_new_worker = false;
  for (uint32_t i = 0; i < n; i++) {
    if (memcmp(&out[i].addr, &a, len) == 0) {
      assert(out[i].worker_id == 2);
      found_a_with_new_worker = true;
    }
  }
  assert(found_a_with_new_worker);

  sfu_room_destroy(&room);
  printf("test_room: OK\n");
  return 0;
}
