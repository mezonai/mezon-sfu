#include "net/zerocopy.h"
#include "util/log.h"

size_t sfu_fanout_send_zc(sfu_ring_t *ring, sfu_packet_t *pkt, const struct sockaddr_storage *dsts, const socklen_t *dst_lens, size_t count) {
  size_t queued = 0;

  for (size_t i = 0; i < count; i++) {
    int rc = sfu_ring_queue_send_zc(ring, pkt, (const struct sockaddr *)&dsts[i], dst_lens[i]);
    if (rc != 0) {
      /* SQ full: flush what we have and let the caller decide
       * whether to retry the remainder or accept partial fan-out. */
      SFU_LOG_WARN("fanout SQ full after %zu/%zu destinations", queued, count);
      break;
    }
    queued++;
  }

  return queued;
}
