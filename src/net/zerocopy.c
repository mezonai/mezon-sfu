#include "net/zerocopy.h"
#include "util/log.h"

size_t sfu_fanout_send_zc(sfu_net_t *ring, sfu_packet_t *pkt, const struct sockaddr_storage *dsts, const socklen_t *dst_lens, size_t count) {
  size_t queued = 0;

  for (size_t i = 0; i < count; i++) {
    int rc = sfu_net_send(ring, pkt, (const struct sockaddr *)&dsts[i], dst_lens[i]);
    if (rc != 0) {
      if (sfu_log_rate_limit("fanout_send_full", 1000000000ULL)) {
        SFU_LOG_WARN("fanout send queue full after %zu/%zu destinations", queued, count);
      }
      break;
    }
    queued++;
  }

  return queued;
}
