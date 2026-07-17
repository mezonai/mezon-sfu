#ifndef SFU_NET_BATCH_H
#define SFU_NET_BATCH_H

#include <liburing.h>

/*
 * Thin wrappers around liburing's batch CQE peek/advance. Kept as their
 * own translation unit because both the dispatcher's recv loop and each
 * worker's send-completion loop use exactly this pattern: pull as many
 * completions as are available in one call (amortizing the ring's memory
 * barrier), process them all, then advance the CQ once -- never
 * advancing (or submitting) after every single packet.
 */

/* Fills cqes[] with up to max_count ready completions without blocking.
 * Returns the number actually filled (0 if none ready). */
unsigned sfu_batch_peek_cqe(struct io_uring *ring, struct io_uring_cqe **cqes, unsigned max_count);

/* Advances the CQ head by `count` after the caller has finished reading
 * cqes[0..count). Must be called exactly once per peek, after processing
 * every entry -- never per-entry. */
void sfu_batch_advance_cqe(struct io_uring *ring, unsigned count);

#endif /* SFU_NET_BATCH_H */
