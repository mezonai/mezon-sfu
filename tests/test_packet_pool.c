#include "memory/packet_pool.h"
#include "memory/refcount.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    sfu_packet_pool_t pp;
    assert(sfu_packet_pool_init(&pp, 16, 1600) == 0);

    sfu_packet_t *pkt = sfu_packet_pool_alloc(&pp);
    assert(pkt != NULL);
    assert(pkt->data != NULL);
    assert(pkt->buf_source == SFU_BUF_SOURCE_POOL);

    uint32_t gen_before = sfu_packet_generation(pkt);

    /* Simulate a 3-way fan-out: retain twice more (now refcount=3),
     * release three times total; only the last release should report
     * "this was the final reference". */
    sfu_packet_retain(pkt, 2);
    assert(sfu_packet_release(pkt) == 0);
    assert(sfu_packet_release(pkt) == 0);
    assert(sfu_packet_release(pkt) == 1);

    sfu_packet_pool_free(&pp, pkt);

    /* Re-allocate and confirm the generation counter advanced, proving
     * a stale (pointer, old_generation) pair from before the free would
     * now be detectable as dead. */
    sfu_packet_t *pkt2 = sfu_packet_pool_alloc(&pp);
    assert(pkt2 != NULL);
    assert(sfu_packet_generation(pkt2) != gen_before);
    sfu_packet_pool_free(&pp, pkt2);

    sfu_packet_pool_destroy(&pp);
    printf("test_packet_pool: OK\n");
    return 0;
}
