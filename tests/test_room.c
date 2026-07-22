#include "room/room.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  sfu_room_t room;

  /* 1. Initialize room */
  assert(sfu_room_init(&room, 1, "test_room") == 0);

  const char *ufrag_a = "ufrag_a";
  const char *ufrag_b = "ufrag_b";
  const char *sdp_a = "v=0\r\no=- 1001 1001 IN IP4 127.0.0.1\r\n";
  const char *sdp_b = "v=0\r\no=- 2002 2002 IN IP4 127.0.0.1\r\n";

  uint32_t a_audio = 1001, a_video = 1002, a_rtx = 1003;
  uint32_t b_audio = 2001, b_video = 2002, b_rtx = 2003;

  /* 2. Publish peer A */
  sfu_room_publish(&room, ufrag_a, 10, sdp_a, strlen(sdp_a), a_audio, a_video, a_rtx);

  /* Peer A should NOT see any other publisher yet */
  uint32_t audio = 0, video = 0, rtx = 0;
  assert(!sfu_room_get_other_publisher_ssrcs(&room, ufrag_a, &audio, &video, &rtx));

  /* 3. Publish peer B */
  sfu_room_publish(&room, ufrag_b, 11, sdp_b, strlen(sdp_b), b_audio, b_video, b_rtx);

  /* Peer A should now see Peer B's SSRCs */
  assert(sfu_room_get_other_publisher_ssrcs(&room, ufrag_a, &audio, &video, &rtx));
  assert(audio == b_audio);
  assert(video == b_video);
  assert(rtx == b_rtx);

  /* Peer B should see Peer A's SSRCs */
  assert(sfu_room_get_other_publisher_ssrcs(&room, ufrag_b, &audio, &video, &rtx));
  assert(audio == a_audio);
  assert(video == a_video);
  assert(rtx == a_rtx);

  /* 4. Update Peer A's SSRCs */
  uint32_t new_a_audio = 1010;
  sfu_room_set_publisher_ssrcs(&room, ufrag_a, new_a_audio, 0, 0);

  /* Peer B should see updated audio SSRC for Peer A, while video/rtx remain unchanged */
  assert(sfu_room_get_other_publisher_ssrcs(&room, ufrag_b, &audio, &video, &rtx));
  assert(audio == new_a_audio);
  assert(video == a_video);
  assert(rtx == a_rtx);

  /* 5. Unpublish Peer B */
  sfu_room_unpublish(&room, ufrag_b);

  /* Peer A should no longer find active external publisher SSRCs */
  assert(!sfu_room_get_other_publisher_ssrcs(&room, ufrag_a, &audio, &video, &rtx));

  /* Cleanup */
  sfu_room_destroy(&room);
  printf("test_room: OK\n");
  return 0;
}
