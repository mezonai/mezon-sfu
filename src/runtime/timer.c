#include "runtime/timer.h"

#include <time.h>

uint64_t sfu_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t sfu_now_ms(void) { return sfu_now_ns() / 1000000ULL; }
uint64_t sfu_now_us(void) { return sfu_now_ns() / 1000ULL; }
