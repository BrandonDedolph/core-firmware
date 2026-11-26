/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/backlightpolicy.c — see backlightpolicy.h.
 */

#include "backlightpolicy.h"

int backlightpolicy_step(int bl_state, uint32_t idle_us, int backlight_secs)
{
    uint32_t dim_us, off_us;
    if (backlight_secs <= 0) {
        dim_us = off_us = 0xFFFFFFFFu;      /* never dim/off               */
    } else {
        dim_us = (uint32_t)backlight_secs * 1000000u;
        off_us = dim_us + BACKLIGHT_OFF_AFTER_DIM_US;
    }
    if (bl_state == BL_FULL && idle_us > dim_us) {
        return BLP_DIM;
    } else if (bl_state == BL_DIM && idle_us > off_us) {
        return BLP_OFF;
    }
    return BLP_STAY;
}

int backlightpolicy_dim_level(int backlight_bright)
{
    int dim_level = backlight_bright / 4;
    if (dim_level < 1) dim_level = 1;
    return dim_level;
}

int backlightpolicy_cpu(int cpu_idled, int bl_state, int playing)
{
    if (cpu_idled && (bl_state != BL_OFF || playing)) {
        return BLP_CPU_BOOST;
    } else if (!cpu_idled && bl_state == BL_OFF && !playing) {
        return BLP_CPU_IDLE;
    }
    return BLP_CPU_STAY;
}
