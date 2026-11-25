/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/util.h — small helpers every app module uses: the idle-task CPU
 * wait, the wrap-correct timed UI window, and a signed decimal to the UART.
 * Moved verbatim out of kernel/main.c.
 */

#ifndef CORE_APP_UTIL_H
#define CORE_APP_UTIL_H

#include <stdint.h>

void cpu_wait_ms(uint8_t ms);
void cpu_wait_us(uint8_t us);

/* ---------------------------------------------------------------------------
 * Timed UI windows (volume overlay, Hold banner, low-battery toast)
 *
 * USEC_TIMER is a free-running 1 MHz counter, so it WRAPS every ~71.6 min. An
 * absolute "now < deadline" compare is wrong across that wrap (the deadline
 * looks like it's in the distant past, or a stale one springs back to life), so
 * every timed check in this file stores a START stamp and compares an ELAPSED
 * difference — (uint32_t)(now - t0) < span — which is wrap-correct. An expired
 * window disarms itself so a later wrap can never re-open it.
 * ------------------------------------------------------------------------- */
typedef struct { uint32_t t0; int armed; } ui_window_t;

void ui_window_arm(ui_window_t *w);
int ui_window_up(ui_window_t *w, uint32_t span, uint32_t now);

void uart_dec(int v);

#endif /* CORE_APP_UTIL_H */
