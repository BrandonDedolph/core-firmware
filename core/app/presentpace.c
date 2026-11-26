/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/presentpace.c — the pacing of run_ui's UI presents. See
 * presentpace.h.
 */
#include <stdint.h>

#include "presentpace.h"

uint32_t present_gap_for(uint32_t cost_us)
{
    uint32_t gap = cost_us * PRESENT_GAP_MULT;
    if (gap < PRESENT_GAP_MIN_US) gap = PRESENT_GAP_MIN_US;   /* <=50 fps: no point going faster    */
    if (gap > PRESENT_GAP_MAX_US) gap = PRESENT_GAP_MAX_US;   /* the old worst-case throttle        */
    return gap;
}

int present_due(uint32_t now, uint32_t last, uint32_t gap)
{
    return (uint32_t)(now - last) >= gap;
}
