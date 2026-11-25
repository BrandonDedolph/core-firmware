/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/nav.c — see nav.h.
 */

#include <stdint.h>

#include "hw/uart.h"
#include "../ui/wheel.h"
#include "settings_glue.h"
#include "albums.h"
#include "nav.h"
#include "nowplaying.h"
#include "lists.h"

/* Bumped whenever a list's CONTENT is rebuilt or the screen stack moves. The
 * partial-repaint path (list_repaint_partial) compares it, so it can never
 * repaint two rows of a list that is no longer the one it last painted — the
 * screen identity and row count alone can coincide (two genres with the same
 * number of songs, reached faster than one throttled repaint window). */
uint32_t g_list_epoch;

/* ---------------------------------------------------------------------------
 * Screen stack
 * ------------------------------------------------------------------------- */
screen_t g_scr[SCR_STACK_MAX];
int      g_scr_n;

/* Both ends of the stack forget the wheel gesture, which is what ui/wheel.h
 * has always said a screen change does. It matters more now than it read:
 * letter mode survives a pause shorter than the plate's hold (1.2 s), so
 * without this a fast spin that ended in a SELECT would carry letter mode —
 * and the plate — onto the screen that SELECT opened, where one detent would
 * jump a letter on a list the user had not even seen yet. */
void      scr_push(screen_t s) { g_list_epoch++;
                                        wheel_accel_reset();
                                        if (g_scr_n < SCR_STACK_MAX) g_scr[g_scr_n++] = s;
                                        else uart_puts("core: scr_push overflow, dropped\n"); }
void      scr_pop(void)        { g_list_epoch++;
                                        wheel_accel_reset();
                                        if (g_scr_n > 1) g_scr_n--; }
screen_t  scr_cur(void)        { return g_scr[g_scr_n - 1]; }
int       scr_is_modal(screen_t s) { return s == SCR_BATTERY || s == SCR_CHARGING; }
/* A modal (BATTERY, CHARGING) over a modal takes its slot instead of a new
 * one: the two are alternatives for the same panel, and popping the top one
 * lands on the real screen underneath, which is what both auto-dismiss paths
 * expect. */
void      scr_push_modal(screen_t s) { if (scr_is_modal(scr_cur())) {
                                                  g_list_epoch++;
                                                  g_scr[g_scr_n - 1] = s;
                                              } else scr_push(s); }

/*
 * Hold MENU: home, from wherever you are. The tap already popped one screen
 * at the down-edge (that is what keeps every back zero-latency), so the hold
 * reads as "back, then home" — deliberate, and the second transition is the
 * one the gesture is for.
 *
 * Returns 1 if anything moved: at the root this is a true no-op, not even a
 * repaint. Everything dropped here is state that belongs to a screen the
 * stack no longer holds — the browser's depth, the root list's wheel
 * remainder, a SELECT press still being timed on Now Playing, the wheel
 * scrubber, and the wheel gesture itself (letter mode outlives a pause now,
 * so a spin that ended in this hold would otherwise arrive at the main menu
 * still stepping letters). The seek holds need nothing: `allowed` goes with
 * the screen and they cancel themselves on the next feed.
 */
int scr_pop_to_root(void)
{
    if (g_scr_n <= 1) {
        return 0;
    }
    if (scr_cur() == SCR_SETTINGS) settings_leave();
    g_scr_n = 1;
    g_list_epoch++;
    wheel_accel_reset();               /* a screen change, as scr_pop is */
    g_dir_depth   = 0;
    g_menu_accum  = 0;
    g_sel_pending = 0;
    if (np_scrubbing()) scrub_exit();
    return 1;
}
