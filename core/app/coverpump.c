/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/coverpump.c — the album list's per-pass cover-load budget. See
 * coverpump.h.
 */
#include <stdint.h>

#include "coverpump.h"

int coverpump_budget(int parked, int locked, uint32_t since_wheel,
                     int idle, uint32_t since_chip, int *held)
{
    int moving = since_wheel < CHIP_WHEEL_SETTLE_US;
    int budget;
    *held = 0;
    if (parked && locked) {
        /* Locked: the user has said they are done with the menus,
         * and the lock parks the drive. Waking it for a cover here
         * was a spin-up and a park per batch. */
        budget = 0;
    } else if (parked && since_wheel < CHIP_SPINUP_QUIET_US) {
        budget = 0;
        *held = 1;
    } else if (moving) {
        budget = 1;
    } else {
        budget = idle ? 6 : 3;
    }
    if (budget > 0 && (idle || since_chip >= 60000u)) return budget;
    return 0;
}
