#ifndef SFU_RUNTIME_TIMER_H
#define SFU_RUNTIME_TIMER_H

#include <stdint.h>

/* Monotonic nanosecond clock -- used for recv timestamps, RTCP interval
 * scheduling, and pacing. A single clock_gettime() call per packet is
 * cheap (vDSO, no syscall trap) so this is safe to call on the hot path.
 *
 * NOTE: this file intentionally holds only the clock primitive for now.
 * The per-core hierarchical timing wheel for RTCP/pacing timers (needed
 * once congestion/pacing.c and rtcp/sender_report.c land -- see the
 * design note on O(1) wheel vs. O(log n) heap for high-churn per-stream
 * timers) is a follow-up addition once those consumers exist. */
uint64_t sfu_now_ns(void);
uint64_t sfu_now_ms(void);

#endif /* SFU_RUNTIME_TIMER_H */
