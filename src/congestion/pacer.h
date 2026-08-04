#ifndef SFU_CONGESTION_PACER_H
#define SFU_CONGESTION_PACER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SFU_PACER_CLASS_AUDIO = 0, /* also RTCP-equivalent control traffic */
  SFU_PACER_CLASS_RTX,       /* retransmissions: below fresh audio */
  SFU_PACER_CLASS_VIDEO_BASE,
  SFU_PACER_CLASS_VIDEO_ENH, /* spatial/temporal enhancement: droppable */
  SFU_PACER_CLASS_COUNT
} sfu_pacer_class_t;

typedef struct sfu_pacer {
  /* Signed byte budget: tokens minus debt. int64 so a burst packet larger
   * than the balance can go negative and throttle what follows. */
  int64_t balance_bytes;
  /* Cap on positive accumulation; bounds burst size after an idle gap. */
  int64_t bucket_cap_bytes;
  /* Token refill rate: pacing_bps/8 bytes per second. */
  uint32_t pacing_bps;
  /* Last refill timestamp, microseconds. 0 = clock not yet started. */
  int64_t last_refill_us;
  bool active;
  int64_t rtx_budget_bytes;
  int64_t rtx_budget_cap_bytes;
  int64_t rtx_last_refill_us;
  uint32_t rtx_budget_bps;
  /* Observability (per subscriber, summed into global metrics by callers). */
  uint64_t sent[SFU_PACER_CLASS_COUNT];
  uint64_t dropped_enh;
  uint64_t rtx_dropped_budget;
} sfu_pacer_t;

void sfu_pacer_init(sfu_pacer_t *p);

/* Arm/disarm pacing. Called on the owning worker when the GCC estimate
 * changes (and once at session setup). bps == 0 disables the pacer: with no
 * estimate (transport-cc not negotiated) forwarding is unpaced, matching the
 * pre-pacer behavior. The current balance is preserved across retunes so a
 * feedback burst cannot reset accumulated debt. */
void sfu_pacer_set_rate(sfu_pacer_t *p, uint32_t bps, int64_t now_us);

/* Admission control for one packet of `bytes` (the wire size including the
 * SRTP auth tag the caller will append). Returns true if the packet may be
 * protected/sent now. May update *inout_now_us with the admission timestamp
 * the caller must record as the TWCC send time.
 *
 * When inactive, always true and *inout_now_us is left untouched. */
bool sfu_pacer_should_send(sfu_pacer_t *p, sfu_pacer_class_t cls, uint32_t bytes, int64_t *inout_now_us);

/* Number of bytes an admission of `bytes` would push the balance negative
 * (0 when it fits without borrowing); used by tests to observe pacing. */
int64_t sfu_pacer_debt_after(const sfu_pacer_t *p, uint32_t bytes, int64_t now_us);

/* RTX time-window budget (CC-16). Returns true and consumes `bytes` from the
 * retransmission budget when the window allows; false (and counts a drop)
 * when the sustained NACK rate has exhausted it. The budget refills at a
 * small fraction of the pacing rate set by set_rate, so retransmissions can
 * never starve fresh media no matter how fast feedback arrives. Unlike the
 * main bucket this one never borrows: excess retransmission requests are
 * dropped (the receiver will re-NACK or PLI). */
bool sfu_pacer_rtx_allow(sfu_pacer_t *p, uint32_t bytes, int64_t now_us);

#endif /* SFU_CONGESTION_PACER_H */
