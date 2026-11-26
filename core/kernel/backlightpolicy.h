/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/backlightpolicy.h — when an idle screen dims and goes out, and
 * when the core may drop to 30 MHz behind it.
 *
 * The policy used to be inline in run_ui's loop (kernel/main.c). It is here
 * now; run_ui keeps the hardware (the timer read, backlight_set, the panel
 * sleep, cpu_boost/cpu_unboost) in the same order at the same place and
 * passes the answers in.
 *
 * THE BACKLIGHT (once per main-loop pass, after the input and the screen's
 * own handling, so a press in this pass has already restamped the idle
 * clock):
 *   - Settings > Backlight is the dim timeout in seconds; 0 (or below) is
 *     "never": neither step ever fires.
 *   - Lit, and idle strictly longer than the timeout -> DIM, at a quarter of
 *     Settings > Brightness, never below 1 (a dimmed panel stays readable).
 *   - Dimmed, and idle strictly longer than the timeout + 15 s -> OFF.
 *   - One step per pass, and the policy never relights: the input sites
 *     (the Hold edge, a button under Hold, any wheel event, the brightness
 *     slider, a resume from suspend, a refused standby) set BL_FULL and
 *     restamp the idle clock themselves, and from there the next dim is a
 *     whole timeout away.
 *   - Nothing else holds the screen up: not playback, not external power,
 *     not the Hold switch (Hold's edge and a button under Hold are inputs;
 *     wheel motion under Hold is not, so pocket friction never keeps the
 *     screen lit). None of those is an input here.
 *
 * THE CPU (the same pass, right after the backlight step, so a screen that
 * went out this pass idles the core this pass): once the screen is fully
 * OFF and nothing is playing (player_playing: the DAC running, NOT
 * player_active, which stays set across a pause), unboost 80 -> 30 MHz.
 * Boost again the instant there is life: the screen anything but OFF, or
 * playback running. It rides the boot path's refcounted boost, so it must
 * stay balanced: an unboost only from un-idled, a boost only from idled,
 * each flipping cpu_idled. A caller that re-boosts behind the policy's back
 * (suspend_to_ram's wake) clears cpu_idled itself, or the policy would
 * boost a second time and the refcount would stick at 2.
 *
 * Pure and freestanding: no hardware access; the caller passes the idle time
 * (its USEC_TIMER read minus the last-input stamp, a uint32_t difference, so
 * the ~71.6 min wrap is harmless). Host-tested in
 * tests/kernel/backlightpolicy_test.c.
 */
#ifndef CORE_KERNEL_BACKLIGHTPOLICY_H
#define CORE_KERNEL_BACKLIGHTPOLICY_H

#include <stdint.h>

/* Backlight states, run_ui's L->bl_state: set by ui_boot, stepped by run_ui's loop. */
enum { BL_OFF, BL_DIM, BL_FULL };

/* Lit -> dimmed after the Settings timeout; dimmed -> off this much later. */
#define BACKLIGHT_OFF_AFTER_DIM_US  (15u * 1000000u)

/* What backlightpolicy_step asks for this pass. */
enum {
    BLP_STAY,             /* nothing                                          */
    BLP_DIM,              /* backlight_set(backlightpolicy_dim_level()), DIM  */
    BLP_OFF,              /* backlight_set(0), OFF (and the panel sleep)      */
};

/* What backlightpolicy_cpu asks for this pass. */
enum {
    BLP_CPU_STAY,         /* nothing                                          */
    BLP_CPU_BOOST,        /* cpu_boost(), cpu_idled = 0                       */
    BLP_CPU_IDLE,         /* cpu_unboost(), cpu_idled = 1                     */
};

/* The backlight step for a screen in `bl_state`, `idle_us` since the last
 * input, under Settings > Backlight `backlight_secs` (<= 0: never). */
int backlightpolicy_step(int bl_state, uint32_t idle_us, int backlight_secs);

/* The dimmed level for Settings > Brightness `backlight_bright`. */
int backlightpolicy_dim_level(int backlight_bright);

/* The CPU step, AFTER this pass's backlight step: `bl_state` is the state it
 * left, `playing` player_playing(). */
int backlightpolicy_cpu(int cpu_idled, int bl_state, int playing);

#endif /* CORE_KERNEL_BACKLIGHTPOLICY_H */
