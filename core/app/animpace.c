/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/animpace.c — the rate limits of run_ui's in-place animations. See
 * animpace.h.
 */
#include <stdint.h>

#include "animpace.h"

int bars_step_due(uint32_t now, uint32_t last)
{
    return (uint32_t)(now - last) >= BARS_STEP_US;
}

int marquee_step_due(uint32_t now, uint32_t last)
{
    return (uint32_t)(now - last) >= MQ_STEP_US;
}
