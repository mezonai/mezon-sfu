#ifndef SFU_CONGESTION_PACER_H
#define SFU_CONGESTION_PACER_H

#include <stdbool.h>
#include <stdint.h>
#include "sfu/datadef.h"

void sfu_pacer_init(sfu_pacer_t *p);
void sfu_pacer_set_rate(sfu_pacer_t *p, uint32_t bps, int64_t now_us);
bool sfu_pacer_should_send(sfu_pacer_t *p, sfu_pacer_class_t cls, uint32_t bytes, bool allow_congestion_drop, int64_t *inout_now_us);
int64_t sfu_pacer_debt_after(const sfu_pacer_t *p, uint32_t bytes, int64_t now_us);
bool sfu_pacer_rtx_allow(sfu_pacer_t *p, uint32_t bytes, int64_t now_us);

#endif /* SFU_CONGESTION_PACER_H */
