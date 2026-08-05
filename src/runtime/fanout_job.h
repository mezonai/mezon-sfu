#ifndef SFU_RUNTIME_FANOUT_JOB_H
#define SFU_RUNTIME_FANOUT_JOB_H

#include "runtime/fanout.h"

/* Executes one cross-worker fanout job on the destination worker. */
void sfu_worker_handle_fanout_job(void *user_data, sfu_fanout_job_t *job);

#endif /* SFU_RUNTIME_FANOUT_JOB_H */
