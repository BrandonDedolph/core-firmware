/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/power.h — the two ways the device goes "off": suspend-to-RAM
 * (suspend_to_ram: the instant-wake sleep, its low-power idle loop, the
 * hold and 30-minute escalations) and the PMU standby (enter_standby: the
 * true off, with its refused-standby recovery). Moved verbatim out of
 * kernel/main.c; the SUSPEND_* build knobs travel with the code.
 */

#ifndef CORE_APP_POWER_H
#define CORE_APP_POWER_H

#include <stdint.h>

/* Set by enter_standby() when the PMU refused the power-down; run_ui reads
 * and clears it. */
extern int g_standby_refused;

int enter_standby(void);
void suspend_to_ram(uint32_t play_down_us);

#endif /* CORE_APP_POWER_H */
