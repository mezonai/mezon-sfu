#include "memory/refcount.h"
#include "util/log.h"

void sfu_packet_debug_dump(const sfu_packet_t *pkt) {
  SFU_LOG_DEBUG("packet idx=%u kbuf=%u len=%u refcount=%u gen=%u", pkt->pool_index, pkt->kbuf_index, pkt->len,
                atomic_load_explicit((_Atomic uint32_t *)&pkt->refcount, memory_order_relaxed), sfu_packet_generation(pkt));
}
