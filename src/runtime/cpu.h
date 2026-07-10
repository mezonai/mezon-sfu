#ifndef SFU_RUNTIME_CPU_H
#define SFU_RUNTIME_CPU_H

#include <pthread.h>

/* Pins the calling thread to a single core. Each worker/dispatcher thread
 * owns its io_uring instance and hashmap shard exclusively -- pinning
 * keeps that state resident in one core's L2 rather than migrating and
 * cold-missing on every scheduler shuffle. Returns 0 on success. */
int sfu_pin_current_thread_to_core(int core_id);

/* Returns the number of online cores, or 1 if it cannot be determined. */
int sfu_online_cpu_count(void);

#endif /* SFU_RUNTIME_CPU_H */
