#include "runtime/cpu.h"
#include "util/log.h"

#include <sched.h>
#include <string.h>
#include <unistd.h>

int sfu_pin_current_thread_to_core(int core_id) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core_id, &set);

  int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (rc != 0) {
    SFU_LOG_WARN("failed to pin thread to core %d: %s", core_id, strerror(rc));
    return -1;
  }
  return 0;
}

int sfu_online_cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}
