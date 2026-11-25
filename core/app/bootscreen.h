/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/bootscreen.h — the boot / loading screen (click-wheel mark,
 * "Core", device line, phase in small caps, optional 2 px progress bar) and
 * the throttled and deferred load bars painted over it. Moved verbatim out
 * of kernel/main.c.
 */

#ifndef CORE_APP_BOOTSCREEN_H
#define CORE_APP_BOOTSCREEN_H

/* `phase` is the small-caps line under the bar; pct < 0 draws no bar. */
void boot_screen_render(const char *phase, int pct);

/* The boot screen with its bar (0..100%), rate-limited: 0% and 100% always
 * paint, anything else within LOAD_BAR_MIN_GAP_US of the last paint is
 * dropped. */
void load_bar(const char *phase, int pct);

/* Deferred bar: load_bar_begin() stamps the start; load_bar_progress() is
 * silent until LOAD_BAR_DELAY_US has passed, then behaves as load_bar(). */
void load_bar_begin(void);
void load_bar_progress(const char *phase, int pct);

#endif /* CORE_APP_BOOTSCREEN_H */
