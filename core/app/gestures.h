/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/gestures.h — run_ui's button gestures, one function per block of
 * its main loop.
 *
 * Each of these was a block inline in run_ui's loop (kernel/main.c). run_ui
 * still calls them at the same points in the same order, and the comment
 * that explains each one stays there, above its call. Each body is the
 * original block with run_ui's loop state reached through `L`. None of them
 * is handed a timer value: every USEC_TIMER read is the block's own.
 *
 * The PLAY arbiter (tap = pause, hold = sleep), the sleep timer and the idle
 * sleep are NOT here: they share one stamp and one suspend_to_ram call, and
 * stay in run_ui beside the suspend wiring.
 */

#ifndef CORE_APP_GESTURES_H
#define CORE_APP_GESTURES_H

#include <stdint.h>

#include "hw/clickwheel.h"
#include "../fs/fat32.h"
#include "ui_loop.h"

/* RIGHT/LEFT by press length (a tap skips, a hold seeks) on the player
 * screens, and MENU's hold (home). Fed from LIVE button state every pass. */
void gesture_seek_menu(ui_loop_t *L);

/* The Hold switch's edge: toggle the input lock, void every press in flight,
 * flash the Hold banner, light the panel. */
void gesture_hold_switch(ui_loop_t *L);

/* The first look at a drained event: swallowed under Hold (a button only
 * re-shows the banner and lights the panel), or the first real touch that
 * ends an On-The-Go or unlock banner. Returns `have_ev` as run_ui is to go
 * on with it: 0 once the event is swallowed. */
int gesture_input_gate(ui_loop_t *L, const wheel_event_t *ev, int have_ev);

/* An event run_ui goes on with, before the per-screen switch: the press that
 * wakes the backlight (swallowed when the screen was off), the click, the
 * battery warning's "seen", a modal's dismissal, and RIGHT/LEFT handed to the
 * seek machines or (off the player screens) RIGHT's jump to Now Playing.
 * Zeroes the parts of `*ev` it consumes. */
void gesture_input_front(ui_loop_t *L, wheel_event_t *ev);

/* SELECT by press length on a list row: the tap acts, the hold adds to (or
 * removes from) On-The-Go. Fed every pass. */
void gesture_row_select(ui_loop_t *L, fat32_t *fs);

/* SELECT by press length on Now Playing: the tap toggles the scrubber, the
 * hold opens the queue. */
void gesture_np_select(ui_loop_t *L);

/* The wheel scrubber's deferred seek, and its lapse back to volume. */
void gesture_np_scrub(ui_loop_t *L);

#endif /* CORE_APP_GESTURES_H */
