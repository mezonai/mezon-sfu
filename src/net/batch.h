#ifndef SFU_NET_BATCH_H
#define SFU_NET_BATCH_H

#include <liburing.h>

unsigned sfu_batch_peek_cqe(struct io_uring *ring, struct io_uring_cqe **cqes, unsigned max_count);
void sfu_batch_advance_cqe(struct io_uring *ring, unsigned count);

#endif /* SFU_NET_BATCH_H */
