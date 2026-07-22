#include "media/transceiver.h"
#include <string.h>

void sfu_transceiver_init(sfu_transceiver_t *t, uint16_t mid, sfu_media_kind_t kind) {
  memset(t, 0, sizeof(*t));

  t->mid = mid;
  t->kind = kind;
  t->direction = SFU_DIRECTION_INACTIVE;
}
