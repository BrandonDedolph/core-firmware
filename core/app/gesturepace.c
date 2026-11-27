/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/gesturepace.c — the timing decisions of run_ui's Now Playing
 * SELECT and wheel scrubber. See gesturepace.h.
 */
#include <stdint.h>

#include "gesturepace.h"

npsel_t npsel_decide(uint32_t now, uint32_t down_us, int down,
                     uint32_t hold_us)
{
    uint32_t sel_held = now - down_us;
    if (down && sel_held >= hold_us) {
        return NPSEL_HOLD;
    } else if (!down) {
        return NPSEL_TAP;
    }
    return NPSEL_WAIT;
}

scrubq_t scrubq_decide(uint32_t now, uint32_t last_us, int dirty,
                       uint32_t commit_us, uint32_t exit_us)
{
    uint32_t quiet = now - last_us;
    if (dirty && quiet >= commit_us) {
        return SCRUBQ_COMMIT;
    } else if (!dirty && quiet >= exit_us) {
        return SCRUBQ_EXIT;
    }
    return SCRUBQ_WAIT;
}
