/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/gestures.c — run_ui's button gestures, moved out of its main loop
 * (kernel/main.c) one block per function. See gestures.h; the comment that
 * explains each block is above its call in run_ui.
 */
#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/clickwheel.h"
#include "hw/backlight.h"
#include "hw/volume.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/backlightpolicy.h"
#include "../ui/gesture.h"
#include "../ui/keyhold.h"
#include "../ui/wheel.h"
#include "../ui/search.h"
#include "../ui/screen_battery.h"
#include "state.h"
#include "util.h"
#include "nav.h"
#include "nowplaying.h"
#include "statusbar.h"
#include "otg.h"
#include "lists.h"
#include "search_glue.h"
#include "az.h"
#include "actions.h"
#include "ui_loop.h"
#include "gesturepace.h"
#include "gestures.h"

void gesture_seek_menu(ui_loop_t *L)
{
    uint32_t nowg = mmio_read32(USEC_TIMER_ADDR);
    uint32_t btn  = g_locked ? 0u : clickwheel_buttons();
    int      on_player = (scr_cur() == SCR_NOWPLAYING ||
                          scr_cur() == SCR_QUEUE);
    int      seekable  = on_player && player_active();
    /* The machine only reads these where a hold is in flight, which
     * means the button is down; player_elapsed_s() costs a 64-bit
     * divide, so don't pay for it on every idle pass. */
    uint32_t el = 0, tot = 0;
    if (btn & (WHEEL_BTN_RIGHT | WHEEL_BTN_LEFT)) {
        el  = player_elapsed_s();
        tot = player_total_s();
    }

    seekhold_action_t ffa = seekhold_feed(&g_ff,
                                          (btn & WHEEL_BTN_RIGHT) != 0,
                                          nowg, seekable, el, tot);
    seekhold_action_t rwa = seekhold_feed(&g_rw,
                                          (btn & WHEEL_BTN_LEFT) != 0,
                                          nowg, seekable, el, tot);
    for (int i = 0; i < 2; i++) {
        seekhold_t *sk = i ? &g_rw : &g_ff;
        switch (i ? rwa : ffa) {
        case SEEKHOLD_SKIP:
            if (i) player_prev(); else player_next();
            hal_volume_set(g_volume);   /* re-apply over codec re-init */
            L->dirty = 1;
            break;
        case SEEKHOLD_AIM:
            /* The hold owns the aim: hand the wheel back to volume if
             * the scrubber had it, and force the transport band (not
             * `dirty`, which would push the whole frame every tick). */
            if (np_scrubbing()) scrub_exit();
            L->np_last = 0xFFFFFFFFu;
            break;
        case SEEKHOLD_COMMIT:
            /* One seek for the whole hold. A refusal needs nothing:
             * the band repaints the live position either way. */
            (void)player_seek_to(seekhold_target(sk));
            L->np_last = 0xFFFFFFFFu;
            break;
        case SEEKHOLD_CANCEL:
            L->np_last = 0xFFFFFFFFu;      /* live position comes back    */
            break;
        case SEEKHOLD_NONE:
            break;
        }
    }

    if (keyhold_feed(&L->menu_key, (btn & WHEEL_BTN_MENU) != 0, nowg,
                     GESTURE_MENU_HOLD_US) == KEYHOLD_HOLD) {
        if (scr_pop_to_root()) L->dirty = 1;   /* at the root: a true no-op */
    }
}

void gesture_hold_switch(ui_loop_t *L)
{
    int held = clickwheel_hold() ? 1 : 0;
    if (held != L->hold_prev) {
        L->hold_prev = held;
        g_locked  = held;
        keyhold_reset(&L->play_key);     /* a press under the switch is void */
        keyhold_reset(&L->menu_key);
        rowsel_drop(&L->row_key);
        seekhold_reset(&g_ff);
        seekhold_reset(&g_rw);
        /* Force the banner to REPAINT for the new state. Without this, a second
         * edge (e.g. on->off within the 1 s window) leaves banner_up set
         * from the first edge, so the render guard (!banner_up) suppresses
         * the new banner and the unlock one never shows. */
        L->banner_up = 0;
        if (banner_hosted()) {
            ui_window_arm(&g_lock_flash);
        } else {
            /* A dark modal: no banner. `dirty` below repaints it, which
             * is how its corner padlock comes and goes. */
            g_lock_flash.armed = 0;
        }
        L->last_input = mmio_read32(USEC_TIMER_ADDR);   /* wake the backlight    */
        if (L->bl_state != BL_FULL) {
            /* A slept panel is lit by the "Panel wake" block, AFTER its
             * first full present has retired the BCM's panel init. */
            if (!L->panel_slept) backlight_set(g_settings.backlight_bright);
            L->bl_state = BL_FULL;
            wheel_accel_reset();      /* don't resume a pre-sleep gesture */
        }
        L->dirty = 1;
    }
}

int gesture_input_gate(ui_loop_t *L, const wheel_event_t *ev, int have_ev)
{
    if (have_ev && g_locked) {
        /* Hold is on: the event is swallowed — DRAINED, so it cannot fire
         * as a stale press the moment Hold comes off (it used to sit in
         * the latch untouched). A BUTTON press re-shows the banner, the
         * reference prototype's blockedByHold, lighting the panel like any
         * other press so the refusal is seen. Wheel motion is dropped
         * silently: pocket friction on the wheel is exactly what Hold is
         * for, and it must not keep the backlight awake. On a dark modal
         * the padlock is already in the corner; the press only lights
         * the panel. */
        if (ev->buttons) {
            L->banner_up = 0;
            if (banner_hosted()) ui_window_arm(&g_lock_flash);
            L->last_input = mmio_read32(USEC_TIMER_ADDR);
            if (L->bl_state != BL_FULL) {
                if (!L->panel_slept) backlight_set(g_settings.backlight_bright);
                L->bl_state = BL_FULL;
                wheel_accel_reset();
                L->dirty = 1;                /* stale panel: the banner's present is full */
            }
        }
        have_ev = 0;
    } else if (have_ev && g_otg_flash.armed && !g_lock_flash.armed &&
               (ev->buttons || ev->wheel_delta)) {
        /* The On-The-Go banner is confirmation, not a modal: the first
         * real touch ends it and THIS pass falls through to the normal
         * render with the event applied. Same rule as the UNLOCKED
         * banner below, and the reason nothing is ever applied behind it. */
        g_otg_flash.armed = 0;
        L->banner_up     = 0;
        L->dirty = 1;
    } else if (have_ev && g_lock_flash.armed && (ev->buttons || ev->wheel_delta)) {
        /* Hold has just come OFF and the unlock banner is still up: it is
         * confirmation, not a modal, so the first touch of the wheel or a
         * button ends it early and THIS pass falls through to the normal
         * render. Real input only: the Hold edge itself arrives through
         * this drain as an event with no buttons and no delta, in the same
         * pass the edge block armed the banner — without the buttons/delta
         * test it dismissed its own banner and the unlock never showed.
         * With the event applied. Without this the banner's block
         * below `continue`s for the rest of its 1 s window, so every scroll
         * and press in it was processed but nothing was drawn. The LOCKED
         * banner is unaffected: input while locked is swallowed above (and
         * a button there re-arms the window), so it never reaches here. */
        g_lock_flash.armed = 0;       /* ui_window_arm's counterpart          */
        L->banner_up = 0;
        L->dirty = 1;
    }
    return have_ev;
}

void gesture_input_front(ui_loop_t *L, wheel_event_t *ev)
{
    if (L->bl_state != BL_FULL) {
        int was_off = (L->bl_state == BL_OFF);
        /* A slept panel is lit by the "Panel wake" block, AFTER its
         * first full present has retired the BCM's panel init. */
        if (!L->panel_slept) backlight_set(g_settings.backlight_bright);
        L->bl_state = BL_FULL;
        wheel_accel_reset();      /* don't resume a pre-sleep gesture */
        L->dirty = 1;                    /* repaint anything drawn while off */
        if (was_off) {                /* swallow the wake press */
            /* Each button voids only its OWN arbiter: a MENU or wheel
             * wake must not claim the next PLAY tap. PLAY keeps the
             * weaker swallow — holding it from a dark screen is still
             * how the device is turned off — while MENU and RIGHT/LEFT
             * are voided outright, because their long actions (home, a
             * seek) would otherwise ride on a press that was only ever
             * meant to light the screen. */
            if (ev->buttons & WHEEL_BTN_PLAY) {
                keyhold_swallow_tap(&L->play_key);   /* ...its release too */
            }
            if (ev->buttons & WHEEL_BTN_MENU)   keyhold_void(&L->menu_key);
            if (ev->buttons & WHEEL_BTN_SELECT) keyhold_void(&L->row_key);
            if (ev->buttons & WHEEL_BTN_RIGHT)  seekhold_void(&g_ff);
            if (ev->buttons & WHEEL_BTN_LEFT)   seekhold_void(&g_rw);
            ev->buttons = 0;
            ev->wheel_delta = 0;
        }
    }
    /* Menu click on a button press (one per down-edge; not on the
     * swallowed backlight-wake press above). Wheel clicks are emitted by
     * wheel_move itself, only when the cursor actually advances a row —
     * so a sub-threshold tick that doesn't move can't click. */
    if (ev->buttons) {
        ui_click();
    }
    /* Charging screen: ANY press just dismisses it, and the press is
     * CONSUMED here — above the global transport block, which otherwise
     * saw the same event and paused/skipped the music you were only
     * trying to get the modal off the screen for. Same swallow pattern as
     * the backlight wake above. */
    /* Any input dismisses a live toast (not consumed — the press was
     * meant for whatever is underneath) and marks the modal seen.
     * Not for a swallowed wake press: that one only lit the screen,
     * and it must not also count as "the user has seen the warning". */
    if (ev->buttons || ev->wheel_delta) {
        battwarn_input(mmio_read32(USEC_TIMER_ADDR));
    }
    if ((scr_cur() == SCR_CHARGING || scr_cur() == SCR_BATTERY) &&
        ev->buttons) {
        scr_pop();
        L->dirty = 1;
        if (ev->buttons & WHEEL_BTN_PLAY) {
            keyhold_swallow_tap(&L->play_key);   /* PLAY's release too */
        }
        /* ...and the press that got the modal off the screen must not
         * also walk the user home or seek, so those are voided whole
         * (same rule as the backlight wake above). */
        if (ev->buttons & WHEEL_BTN_MENU)   keyhold_void(&L->menu_key);
        if (ev->buttons & WHEEL_BTN_SELECT) keyhold_void(&L->row_key);
        if (ev->buttons & WHEEL_BTN_RIGHT)  seekhold_void(&g_ff);
        if (ev->buttons & WHEEL_BTN_LEFT)   seekhold_void(&g_rw);
        ev->buttons     = 0;
        ev->wheel_delta = 0;
    }
    /* RIGHT/LEFT are transport ONLY on the player screens (Now Playing
     * and the queue view, which lists the live queue with the playing
     * row marked), and WHAT the transport is comes from press length
     * in the seekhold block above, not from here — a tap skips, a hold
     * seeks. This drain has two jobs left. On the player screens it
     * hands over a down-edge the live sampler never got to see. On
     * every other screen it owns the press outright: a skip from a
     * list you were merely browsing changed the music under you, so
     * instead RIGHT jumps to Now Playing — PUSHED over the current
     * screen, so MENU from there lands back exactly where you were
     * (inside an album's tracklist, mid-browse) — and LEFT does
     * nothing (MENU is already "back" on every screen; a second back
     * key is a second convention). Nothing here ever runs on a press
     * that woke the backlight, dismissed a modal or landed under Hold
     * — all three zeroed ev.buttons. */
    if (scr_cur() == SCR_NOWPLAYING || scr_cur() == SCR_QUEUE) {
        /* The latch's promise, kept for the transport too. A tap whose
         * press AND release both fell inside one blocked pass — the
         * second RIGHT of a double-skip, landing while player_next()
         * opens a file on a drive the spin-down parked — is never seen
         * by the 100 Hz live sampler the seek machines read, but the
         * tick latched its down-edge and it is in this event. Hand it
         * over; the machine reports it as a skip on the next feed, so
         * the skip keeps exactly one implementation. A button still
         * down is handed over too: the machine latches the edge and
         * judges it at that next feed — still down, it owns the press
         * as a normal hold; already up, the whole press fell in this
         * gap and it was a tap. */
        uint32_t live = clickwheel_buttons();
        if (ev->buttons & WHEEL_BTN_RIGHT) {
            seekhold_missed_tap(&g_ff, (live & WHEEL_BTN_RIGHT) != 0);
        }
        if (ev->buttons & WHEEL_BTN_LEFT) {
            seekhold_missed_tap(&g_rw, (live & WHEEL_BTN_LEFT) != 0);
        }
    } else if (ev->buttons & WHEEL_BTN_RIGHT) {
        /*
         * Off the player screens this press is spent on whatever the
         * drain does with it and is never a transport, so the seek
         * machine is told so UNCONDITIONALLY, before anything else
         * decides what the press was for.
         *
         * It has to be unconditional because the two things that look
         * at this press run in a fixed order and can straddle it: the
         * machine is fed from the LIVE button state at the top of the
         * pass, and this drain reads the TICK-LATCHED event further
         * down. A press that begins between those two — any press
         * during a long pass, a cover read, a spin-up, a present —
         * was not down when the machine sampled, so it latched
         * nothing; the drain then pushes Now Playing, and the NEXT
         * pass shows the machine a fresh down-edge with `allowed`
         * now 1. Without the void it owns that press outright: a
         * release under GESTURE_SEEK_HOLD_US skips the track the user
         * was only trying to look at, and a hold seeks it. The void
         * is what makes the press dead for the rest of its life.
         *
         * It touches only the machine, never ev.buttons, so Search's
         * picker still gets its RIGHT below: the space bar is the one
         * place off the player screens where the press means something
         * to a screen, and a 39-cell ring has nowhere else to put one.
         * (In PICK the press is safe from the machine either way — it
         * began on a non-player screen, so the feed latches
         * allowed_at_down = 0 — but a hold that reaches Now Playing by
         * some other route must not wake up as a seek.) In RESULTS the
         * global rule below applies again.
         */
        seekhold_void(&g_ff);
        if (player_active() &&
            !(scr_cur() == SCR_SEARCH && g_search.mode == SEARCH_PICK)) {
            /* Now Playing is never beneath a list today (only MENU or a
             * SELECT-hold leave it, and modals eat every press), so this
             * is insurance against a future screen that could sit above
             * it: a second Now Playing entry would make MENU look dead. */
            int np_on_stack = 0;
            for (int i = 0; i < g_scr_n; i++) {
                if (g_scr[i] == SCR_NOWPLAYING) { np_on_stack = 1; break; }
            }
            if (!np_on_stack) {
                scr_push(SCR_NOWPLAYING);
                L->np_first = 1;
                L->dirty    = 1;
            }
            /* consumed: not a list press */
            ev->buttons = (uint8_t)(ev->buttons & ~WHEEL_BTN_RIGHT);
            /* The switch below runs on the NEW top. A thumb landing on
             * the ring to press RIGHT can latch a sub-detent delta in
             * the same tick; on Now Playing that reads as a volume
             * nudge (+ the plate). The press was a jump, nothing else. */
            ev->wheel_delta = 0;
        }
    }
}

void gesture_row_select(ui_loop_t *L, fat32_t *fs)
{
    uint32_t nowr  = mmio_read32(USEC_TIMER_ADDR);
    int      rdown = !g_locked &&
                     (clickwheel_buttons() & WHEEL_BTN_SELECT) != 0;
    keyhold_action_t ract = keyhold_feed(&L->row_key, rdown, nowr,
                                         SEL_HOLD_US);
    if (g_rowsel.pending &&
        ((int)scr_cur() != g_rowsel.scr || g_locked ||
         g_list_epoch != g_rowsel.epoch ||
         rowsel_sub() != g_rowsel.sub)) {
        /* The press no longer belongs to the row it started on: MENU
         * popped the screen, the list was rebuilt under it, or Hold
         * engaged mid-press (which zeroes clickwheel_buttons() and
         * would read below as a release). Drop it. */
        rowsel_drop(&L->row_key);
    } else if (g_rowsel.pending) {
        if (ract == KEYHOLD_TAP) {
            g_rowsel.pending = 0;
            int r = row_select_tap(fs);
            if (r & ROWSEL_DIRTY)   L->dirty = 1;
            if (r & ROWSEL_PLAYING) L->np_first = 1;
        } else if (ract == KEYHOLD_HOLD) {
            g_rowsel.pending = 0;
            if (row_select_hold(fs)) L->dirty = 1;
        }
    }
}

void gesture_np_select(ui_loop_t *L)
{
    if (g_sel_pending && (scr_cur() != SCR_NOWPLAYING || g_locked)) {
        /* The press no longer belongs to Now Playing: MENU popped it, the
         * track ended and truncated the stack, or Hold engaged mid-press.
         * Hold is the nasty one — it zeroes clickwheel_buttons(), which
         * reads below as a release and would scrub_enter() while locked;
         * MENU-then-release used to toggle the scrubber on whatever
         * screen was current, and the next Now Playing wheel turn then
         * seeked instead of changing the volume. Drop it, and the
         * scrubber with it. */
        g_sel_pending = 0;
        if (np_scrubbing()) {
            scrub_exit();
            L->np_last = 0xFFFFFFFFu;
        }
    }
    if (g_sel_pending) {
        uint32_t nows = mmio_read32(USEC_TIMER_ADDR);
        int      down = (clickwheel_buttons() & WHEEL_BTN_SELECT) != 0;
        npsel_t  sel  = npsel_decide(nows, g_sel_down_us, down, SEL_HOLD_US);
        if (sel == NPSEL_HOLD) {
            g_sel_pending = 0;
            scrub_exit();
            g_queue_sel   = player_queue_current();
            g_queue_accum = 0;
            scr_push(SCR_QUEUE);
            L->dirty = 1;
        } else if (sel == NPSEL_TAP) {
            g_sel_pending = 0;
            if (np_scrubbing()) scrub_exit();
            else                scrub_enter();
            /* Nothing above the transport band knows about the scrubber
             * (nowplaying_render only consults it there), so a forced
             * transport repaint is the whole change — no `dirty`. */
            L->np_last = 0xFFFFFFFFu;
        }
    }
}

void gesture_np_scrub(ui_loop_t *L)
{
    if (np_scrubbing()) {
        uint32_t nowq = mmio_read32(USEC_TIMER_ADDR);
        scrubq_t sq   = scrubq_decide(nowq, g_scrub_last_us, g_scrub_dirty,
                                      SCRUB_COMMIT_US, SCRUB_EXIT_US);
        if (sq == SCRUBQ_COMMIT) {
            g_scrub_dirty = 0;
            if (player_seek_to(g_scrub_target_s) != 0) {
                g_scrub_target_s = player_elapsed_s();   /* refused: snap back */
            }
            L->np_last = 0xFFFFFFFFu;          /* transport band only, as above */
        } else if (sq == SCRUBQ_EXIT) {
            scrub_exit();
            L->np_last = 0xFFFFFFFFu;
        }
        if (!player_active()) {
            scrub_exit();                    /* track ended under the scrubber */
        }
    }
}
