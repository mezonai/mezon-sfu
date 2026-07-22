#ifndef SFU_MEDIA_SUBSCRIPTION_H
#define SFU_MEDIA_SUBSCRIPTION_H

#include <stdbool.h>
#include <stdint.h>

struct sfu_peer;
struct sfu_media_source;

typedef struct sfu_subscription {
  struct sfu_peer *subscriber;

  struct sfu_media_source *source;

  uint16_t receiver_mid_audio;

  uint16_t receiver_mid_video;

  bool active;

} sfu_subscription_t;

#endif  // SFU_MEDIA_SUBSCRIPTION_H
