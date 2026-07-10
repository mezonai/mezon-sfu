#include "net/batch.h"

unsigned sfu_batch_peek_cqe(struct io_uring *ring,
                             struct io_uring_cqe **cqes,
                             unsigned max_count) {
    return io_uring_peek_batch_cqe(ring, cqes, max_count);
}

void sfu_batch_advance_cqe(struct io_uring *ring, unsigned count) {
    if (count > 0) {
        io_uring_cq_advance(ring, count);
    }
}
