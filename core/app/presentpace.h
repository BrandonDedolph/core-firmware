/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/presentpace.h — how far apart run_ui's UI presents must be while
 * a song is loaded, and whether the next one may go out this pass.
 *
 * Both used to be inline: the gap in present_gap_us (app/paint.c, which
 * still owns the measured cost, g_present_cost_us, and passes it in), the
 * comparison in run_ui's render pass (render_tick, app/paint.c, which keeps
 * its timer reads and L->last_present and passes them in):
 *   - The gap is PRESENT_GAP_MULT times what the last full-frame present
 *     cost, clamped to [PRESENT_GAP_MIN_US, PRESENT_GAP_MAX_US]. The
 *     multiply is uint32_t, as it always was: a cost above 2^30 us wraps
 *     before the clamp (no present costs 18 minutes).
 *   - A present is due once at least the gap has passed since the last one.
 *
 * Pure and freestanding: no hardware access. `now` is the caller's
 * USEC_TIMER read and `last` the stamp of the last present; the difference is
 * taken as uint32_t, so the ~71.6 min wrap is harmless. Host-tested in
 * tests/app/presentpace_test.c.
 */
#ifndef CORE_APP_PRESENTPACE_H
#define CORE_APP_PRESENTPACE_H

#include <stdint.h>

#define PRESENT_GAP_MULT        4u    /* the gap is this many last presents  */
#define PRESENT_GAP_MIN_US  20000u    /* <=50 fps: no point going faster     */
#define PRESENT_GAP_MAX_US 150000u    /* the old worst-case throttle         */

/* The gap to keep between presents after one that cost `cost_us`. */
uint32_t present_gap_for(uint32_t cost_us);

/* 1 when a present may go out: `gap` or more since `last`. */
int present_due(uint32_t now, uint32_t last, uint32_t gap);

#endif /* CORE_APP_PRESENTPACE_H */
