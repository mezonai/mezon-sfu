#ifndef SFU_MEDIA_GRAPH_H
#define SFU_MEDIA_GRAPH_H

#include "media/source.h"
#include "media/subscription.h"

#define SFU_MAX_MEDIA_SOURCES 256
#define SFU_MAX_SUBSCRIPTIONS 4096

typedef struct sfu_media_graph {
  sfu_media_source_t sources[SFU_MAX_MEDIA_SOURCES];

  uint32_t source_count;

  sfu_subscription_t subscriptions[SFU_MAX_SUBSCRIPTIONS];

  uint32_t subscription_count;

} sfu_media_graph_t;

void sfu_media_graph_init(sfu_media_graph_t *g);

#endif  // SFU_MEDIA_GRAPH_H
