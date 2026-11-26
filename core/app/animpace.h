/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/animpace.h — whether run_ui's in-place animations are due to step
 * this main-loop pass.
 *
 * The rate limits used to be inline in run_ui's loop (kernel/main.c). The
 * ticks they gate (detail_bars_tick in app/albums.c, queue_bars_tick in
 * app/lists.c, marquee_tick in app/marquee.c) keep their timer read and
 * their stamp, and pass both in:
 *   - The now-playing bars (album detail and queue): every 110 ms, ~9 fps.
 *     One stamp, L->last_bars, is shared by the two screens.
 *   - The marquee: every 33 ms, ~30 fps.
 * A step is due once at least the period has passed since the last one.
 *
 * Pure and freestanding: no hardware access. `now` is the caller's
 * USEC_TIMER read and `last` the stamp of the last step; the difference is
 * taken as uint32_t, so the ~71.6 min wrap is harmless. Host-tested in
 * tests/app/animpace_test.c.
 */
#ifndef CORE_APP_ANIMPACE_H
#define CORE_APP_ANIMPACE_H

#include <stdint.h>

#define BARS_STEP_US  110000u    /* now-playing bars, ~9 fps  */
#define MQ_STEP_US     33000u    /* marquee scroll, ~30 fps   */

/* 1 when the now-playing bars should be redrawn: BARS_STEP_US or more since
 * `last`. */
int bars_step_due(uint32_t now, uint32_t last);

/* 1 when the marquee should be redrawn: MQ_STEP_US or more since `last`. */
int marquee_step_due(uint32_t now, uint32_t last);

#endif /* CORE_APP_ANIMPACE_H */
