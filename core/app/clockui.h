/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/clockui.h — the clock as the UI reads it: the local epoch and
 * civil time (software clock plus the stored offset and Daylight Saving),
 * the title-strip clock text, and the re-anchor of the software clock from
 * the RTC chip. Moved verbatim out of kernel/main.c; the clock state itself
 * lives in state.h.
 */

#ifndef CORE_APP_CLOCKUI_H
#define CORE_APP_CLOCKUI_H

#include <stdint.h>

#include "../kernel/datetime.h"

int clock_local_epoch(uint32_t *local);
int clock_local_now(datetime_t *out);
void clock_title_text(char *buf, int buf_sz);
void clock_resync(const char *why, int trust_timer);

#endif /* CORE_APP_CLOCKUI_H */
