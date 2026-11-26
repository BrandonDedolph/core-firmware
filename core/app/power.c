/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/power.c — see power.h.
 */

#include <stdint.h>

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/ata.h"
#include "hw/clickwheel.h"
#include "hw/backlight.h"
#include "hw/battery.h"
#include "hw/power.h"
#include "hal.h"
#include "../player/player.h"
#include "../kernel/timer.h"
#include "../kernel/clock.h"
#include "../kernel/console.h"
#include "../kernel/cfg_commit.h"
#include "../ui/jackwatch.h"
#include "state.h"
#include "util.h"
#include "clockui.h"
#include "persist.h"
#include "battery_ui.h"
#include "resume.h"
#include "paint.h"
#include "power.h"

/* Set by enter_standby() when the PMU refused the power-down and the device
 * was brought back instead: run_ui reads and clears it to resync its
 * backlight/idle state with the screen enter_standby already relit. */
int g_standby_refused;

/*
 * Quiesce and enter PMU deep-sleep standby — the true "off" (holding PLAY
 * past ~5 s, the suspend timeout, or the battery policy's SHUTOFF edge).
 * A button press wakes the device by re-running the boot path (a cold boot
 * of the firmware, not a resume).
 *
 * Everything that draws is put away BEFORE the rail cut, in the order the
 * hardware wants: the transport and the codec (hal_audio_close powers the
 * WM8758 down and gates the audio clocks), then the settings write while the
 * drive is still spinning, then the drive itself (cache flushed, heads
 * parked, spun down — 04-ata.md: "safe to cut power after this returns"),
 * then the panel (black frame, backlight off, LCD_SLEEP). The PMU write is
 * last. The settings write goes through the normal battery gate: below the
 * disk-safe line it is refused, and the DISKSAFE-edge flush that already
 * happened is what persists (see CFG_COMMIT_LAST).
 *
 * RETURNS -1 IF THE PMU REFUSED. power_standby() gives up after a bounded
 * number of I2C retries rather than hang, and this used to fall into a
 * for(;;) regardless — a dark, dead device, reached by the user asking to
 * turn it off. Now the refusal is recovered here: panel woken, the current
 * screen repainted (the present absorbs the panel init), backlight restored,
 * and the caller carries on. The player has been STOPPED by then, not paused
 * — that is the price of the refusal, and the caller must not resume it.
 * On the device this path has never been seen to fire; it is the I2C-wedged
 * case, and the recovery sequence is UNVERIFIED on hardware.
 */
static void suspend_lowpower_leave(void);

/* DEVICE RESULT 2026-09-13: OFF. The idle-path twin (PANEL_SLEEP_AT_IDLE)
 * came back WHITE on the first wake on the real 5.5G — the LCD_SLEEP →
 * lcd_wake → absorbed first present sequence does not bring the panel back
 * as modelled. Same pair here, so off until that is understood on the
 * bench. The LED is still off across a suspend; the panel merely holds a
 * black frame instead of being slept. */
#ifndef SUSPEND_PANEL_SLEEP
#define SUSPEND_PANEL_SLEEP 0
#endif

int enter_standby(void)
{
    /* If a suspend brought us here (its hold-escalation, its 30-minute
     * escalation, or the SHUTOFF edge inside its battery sample), the PLL
     * is parked, the tick is at 10 Hz and SER0/PWM/I2C are gated. Every
     * step below — the codec's I2C writes, the settings write on a boosted
     * ATA transfer, the BCM frame, and the PMU's own I2C command — was
     * calibrated at the PLL operating points, so come back first. No-op
     * from the main loop. */
    suspend_lowpower_leave();

    /* BEFORE player_stop(): once the transport is torn down there is no track
     * name and no elapsed clock left to record. */
    resume_capture();
    player_stop();
    hal_audio_close();                    /* codec rails off, audio clocks gated */
    settings_commit(1);                   /* persist while the drive still spins */
    otg_commit(CFG_COMMIT_FORCE);         /* ...and the On-The-Go list with it */
    uart_puts("core: standby: entering\n");
    evlog_commit(CFG_COMMIT_FORCE);       /* the log's last block, FINAL; same
                                           * gate — refused below disk-safe, as
                                           * the settings commit above is    */
    if (!ata_is_parked()) {
        ata_standby();                    /* flush + park + spin down. Not
                                           * ata_sleep(): the rail cut follows
                                           * and STANDBY is the device-proven
                                           * path; a suspend has already slept
                                           * it (parked) by the time it gets here */
    }
    console_clear(0xFFFF);                /* blank BEFORE the power cut so no */
    lcd_present_fb(console_framebuffer()); /* stale colour lingers on the panel.
                                           * WHITE, not black: with the LED off a
                                           * black frame on this transflective
                                           * panel is a dark field with the pixel
                                           * grid showing in ambient light; white
                                           * is what "off" looks like (device,
                                           * 2026-09-13)                        */
    cpu_wait_ms(80);                      /* let the BCM push the frame        */
    backlight_set(0);
#if SUSPEND_PANEL_SLEEP
    lcd_sleep();                          /* panel off; the BCM stays alive   */
#endif
    (void)power_standby();                /* PMU cuts power — normally no return */

    /*
     * Still here: the PMU never took the command. Bring the device back to a
     * usable state rather than leave it dark. Wake the panel and push ONE
     * frame BEFORE the backlight: that first present is the one the BCM
     * answers with its ~500 ms panel init, and lighting the LED over it is
     * the white flash (02-lcd.md).
     */
    uart_puts("core: standby REFUSED by the PMU; staying up\n");
    lcd_wake();
    paint_current_screen();
    lcd_present_fb(console_framebuffer());
    backlight_set(g_settings.backlight_bright);
    g_standby_refused = 1;
    return -1;
}

/*
 * Suspend: the seamless "off". Keeps the CPU + RAM alive so wake RESUMES the
 * running firmware instantly (no cold boot) — and puts everything else away:
 *
 *   audio     paused; the codec powers itself down through player_pump()'s
 *             persistent-pause timeout, which the idle loop keeps calling;
 *   settings  written now (forced), with the resume position;
 *   drive     ata_sleep(): cache flushed, heads parked, platters down, then
 *             SLEEP so the interface logic is off too — a reset wakes it;
 *   panel     black frame, backlight off, then LCD_SLEEP (SUSPEND_PANEL_SLEEP;
 *             the BCM stays powered so no firmware re-upload on wake);
 *   SoC       boost released, then suspend_lowpower_enter(): SER0, PWM0 and
 *             I2C clocks gated in DEV_EN (each re-gates itself on use),
 *             TIMER1 at 10 Hz, and the PLL disabled and unpowered with the
 *             bus on the 24 MHz crystal (kernel/clock.c clock_suspend).
 *
 * While suspended the loop samples the battery on the main loop's 5 s
 * cadence and runs the same DISKSAFE / SHUTOFF policy, so a forgotten device
 * flushes and powers off cleanly instead of deep-discharging to the PMU's
 * hard cut; and on battery it escalates to a real PMU standby on its own
 * after SUSPEND_TO_STANDBY_US. Holding the trigger PLAY past ~5 s escalates
 * at once. `play_down_us` is when the hold began, for that escalation.
 *
 * Any button wakes it: reset + spin the drive up, wake the panel, repaint,
 * present (that present retires the panel's wake-init before returning),
 * THEN backlight, then resume — only if the headphone jack is not known to
 * be empty. A refused PMU standby from any of the escalations falls through
 * this same wake path with the player stopped and nothing to resume.
 *
 * What still draws: the CPU at 24 MHz and the SDRAM (the wake is what they
 * buy — the doc's 32 kHz point is for a core that has stopped), the OPTO
 * block (the wake source), the BCM, and whatever the ROM's undocumented
 * DEV_EN bits (USB/FireWire/IDE) keep clocked. That is why the escalation
 * exists. NOT MEASURED on the device: the suspend draw before or after any
 * of this, whether the panel comes back from LCD_SLEEP
 * (SUSPEND_PANEL_SLEEP has the rollback), the drive's post-SLEEP reset
 * wake, and whether the I2C controller and the wheel come back cleanly
 * from a gate / a crystal-clocked spell — all first-flash items.
 */
/*
 * Put the LCD PANEL to sleep for the suspend, not just the backlight.
 *
 * lcd_sleep() issues the BCM's LCD_SLEEP (panel driver off; the BCM itself
 * stays powered and bootstrapped, so no firmware re-upload is needed). The
 * hazard is on the way back: the first LCD_UPDATE after a wake carries a
 * ~500 ms panel init, and light behind the panel before that update has
 * retired is the solid-white screen (02-lcd.md). The wake path below does
 * lcd_wake(); paint; present; backlight in that order, and the present does
 * not return until the init has retired (hal/hw/lcd.c bcm_frame_commit) —
 * which is what makes this safe to ship at 1.
 *
 * UNVERIFIED ON THE DEVICE as of 2026-09-13. If the screen comes back WHITE
 * after a suspend, set this to 0: the panel then stays driven-black for the
 * suspend exactly as before (backlight off only), and nothing else changes.
 */

/*
 * How long a suspend may last on battery before it escalates to a real PMU
 * standby (enter_standby). Suspend keeps the CPU, RAM, PLL and the 100 Hz
 * tick alive so that wake is instant; that is the right trade for "off for
 * a minute", and the wrong one for "off overnight", where the same draw
 * just takes the cell to the PMU's hard cut with nothing saved. Not applied
 * while on external power: a docked device can sit suspended for as long
 * as it likes. The cost of escalating is a cold boot on the next wake.
 */
/* How long PLAY must stay held AFTER the screen went dark before the hold
 * escalates to a PMU power-off (in addition to ~5 s from the down-edge). */
#ifndef SUSPEND_ESCALATE_DARK_US
#define SUSPEND_ESCALATE_DARK_US  2500000u                /* 2.5 s */
#endif

#ifndef SUSPEND_TO_STANDBY_US
#define SUSPEND_TO_STANDBY_US  (30u * 60u * 1000000u)     /* 30 minutes */
#endif

/* Idle-loop period while suspended, and the tick rate the loop runs under.
 * Nothing in the loop needs to be prompt: the tick samples the wheel into
 * the latch regardless, so a press is seen within one tick period, and the
 * battery sample is on its own 5 s cadence. The loop period matches the
 * tick period, so a suspended core wakes ~20 times a second at most (once
 * for the tick, once for the countdown) instead of ~110; a press shorter
 * than one tick period can fall between samples, which is why "hold any
 * button" is the wake gesture. */
#define SUSPEND_IDLE_MS        100u
#define SUSPEND_TICK_HZ        10u

/*
 * The suspend operating point, entered once the drive, panel and backlight
 * are down (kernel/clock.c, kernel/timer.c):
 *
 *   clock_gate_suspend   DEV_EN: SER0, PWM0 and I2C clocks off — each block
 *                        re-gates itself on its next use, so a UART line,
 *                        an I2C transaction or a click still works;
 *   timer_set_rate(10)   TIMER1 at 10 Hz (the tick counter keeps its 10 ms
 *                        unit; only the wheel sampling coarsens);
 *   clock_suspend        bus onto the 24 MHz crystal, PLL disabled and
 *                        unpowered. Refused (logged) if a boost is still
 *                        held — nothing in this file should be holding one
 *                        by then.
 *
 * In that order, so the core clock is the last thing to move. Left in the
 * reverse order. Both are idempotent through g_suspend_lp so the battery
 * sample, the escalations and the wake path can each call them without
 * caring who went first — and so timer_set_rate is not re-issued on every
 * loop pass, which at a 100 ms period would restart the countdown before
 * it ever fired.
 */
static int g_suspend_lp;

/*
 * DEVICE BISECT, 2026-09-13. With all three of these on, a suspend went dark
 * and never woke (no button, no charger; Menu+Select only), and so did the
 * 5 s hold — its escalation runs from inside the suspend loop, so a loop
 * that cannot see the wheel never escalates either. Panel sleep was already
 * off for that flash. Each piece is switchable so a flash can turn one on
 * at a time; all OFF is the known-good shape (the 2026-09-10 suspend, plus
 * ATA SLEEP).
 *
 * RESULT (three flashes, same evening): all off — wakes. Tick + gates on,
 * PLL off — dark. Gates ONLY — dark. So the CLOCK GATES are what break the
 * wake: the wheel was still alive when the PLAY release was seen (gates
 * already applied), so the kill is later — the 5 s battery cycle un-gates
 * and re-gates SER0/PWM/I2C (the I2C re-gate is a full reset-pulse
 * bring-up), and something in that round trip stops the wheel or the tick.
 * Not yet split per block. The 10 Hz tick was never tested ALONE; the PLL
 * park never tested at all. Both stay off until the on-disk event log can
 * show what the loop did last. Ship: all three 0.
 */
#ifndef SUSPEND_PARK_PLL
#define SUSPEND_PARK_PLL     0
#endif
#ifndef SUSPEND_SLOW_TICK
#define SUSPEND_SLOW_TICK    0
#endif
#ifndef SUSPEND_GATE_CLOCKS
#define SUSPEND_GATE_CLOCKS  0
#endif

static void suspend_lowpower_enter(void)
{
    if (g_suspend_lp) {
        return;
    }
    g_suspend_lp = 1;
#if SUSPEND_GATE_CLOCKS
    clock_gate_suspend();
#endif
#if SUSPEND_SLOW_TICK
    timer_set_rate(SUSPEND_TICK_HZ);
#endif
#if SUSPEND_PARK_PLL
    if (clock_suspend() != 0) {
        /* The UART re-gates itself for this line. */
        uart_puts("core: suspend: PLL park refused (boost held or DMA live)\n");
    }
#endif
}

static void suspend_lowpower_leave(void)
{
    if (!g_suspend_lp) {
        return;
    }
    g_suspend_lp = 0;
#if SUSPEND_PARK_PLL
    clock_resume();
#endif
#if SUSPEND_SLOW_TICK
    timer_set_rate(HZ);
#endif
#if SUSPEND_GATE_CLOCKS
    clock_gate_resume();
#endif
}

void suspend_to_ram(uint32_t play_down_us)
{
    wheel_event_t drain;
    uint32_t suspend_t0 = mmio_read32(USEC_TIMER_ADDR);
    int was_playing = player_active() && !player_paused();
    if (was_playing) {
        player_pause();                   /* silence + stop feeding the disk */
    }
    /* Last chance to persist: after this the drive is parked and the user may
     * never wake the device again (battery pull, dead cell). Forced, so a
     * change made 1 s ago is not lost to the debounce. The position goes in
     * the same write — the pause above froze the elapsed clock, so this is the
     * exact second the user stopped listening. */
    resume_capture();
    settings_commit(1);
    otg_commit(CFG_COMMIT_FORCE);
    /* The log's last chance too, in the same breath: what is pending goes
     * out as a FINAL block while the drive is still up (the forced flush
     * wakes it if a parked-drive commit above did not). Narrated first, so
     * the block records that a suspend was entered. */
    uart_puts("core: suspend: entering\n");
    evlog_commit(CFG_COMMIT_FORCE);
    cpu_unboost();                        /* the boost refcount is >=1 here (we are
                                           * entered from BL_FULL), so without this
                                           * the "sleeping" device holds the 80 MHz
                                           * operating point for the whole suspend */
    /* DEVICE 2026-09-13: STANDBY, not SLEEP. Every short-hold suspend today
     * went dark and "never woke" while the 5 s power-off (which never
     * touches the drive) came back fine. SLEEP needs a bus reset to recover
     * and the wake path issues one BEFORE repainting; if this drive does
     * not answer it, the wake sits in the 31 s reset budget with the screen
     * dark. STANDBY is the device-proven park; a plain read spins it up. */
#ifndef SUSPEND_ATA_SLEEP
#define SUSPEND_ATA_SLEEP 0
#endif
#if SUSPEND_ATA_SLEEP
    ata_sleep();                          /* flush, park, spin down, interface off */
#else
    if (!ata_is_parked()) {
        ata_standby();                    /* park + spin down, interface stays up */
    }
#endif
    /* Clear to black BEFORE cutting the backlight, so the transflective panel
     * doesn't faintly ghost the last UI in ambient light while asleep. Wake
     * repaints the real screen while the backlight is still off (below), so the
     * blank is never seen as a flash. WHITE, not black: with the LED off a
     * black frame shows as a dark field with the pixel grid visible in
     * ambient light — "random black pixels" (device, 2026-09-13); a white
     * frame is what an iPod looks like when it is off. */
    console_clear(0xFFFF);
    lcd_present_fb(console_framebuffer());
    backlight_set(0);
#if SUSPEND_PANEL_SLEEP
    lcd_sleep();                          /* panel driver off; drains the black
                                           * frame first, then LCD_SLEEP     */
#endif
    suspend_lowpower_enter();             /* gates, 10 Hz tick, PLL parked */

    /* Wait for the trigger PLAY hold to release (so it can't instantly wake us).
     * Held past ~5s total => a real power-down instead. If the PMU refuses
     * that, enter_standby() has already stopped the player and repainted:
     * skip the idle wait and fall through to the wake path, which finishes
     * the job (release-wait, re-boost, drive spin-up) without resuming. */
    /* Escalation is timed from BOTH the down-edge (~5 s of hold, the
     * documented gesture) AND from the moment the screen went dark. The
     * teardown above is not instant: a pending settings change on a parked
     * drive is a spin-up (seconds) + write + FLUSH + SLEEP before the
     * backlight drops. Timed from the down-edge alone, "hold until it goes
     * dark, then let go" — the natural gesture — arrived here already past
     * 5 s with PLAY down for one more tick, and turned the sleep the user
     * asked for into a PMU power-off. */
    uint32_t dark_us = mmio_read32(USEC_TIMER_ADDR);
    int standby_refused = 0;
    int jack_pulled = 0;              /* the headphones came out while asleep */
    while (clickwheel_buttons() & WHEEL_BTN_PLAY) {
        uint32_t nowh = mmio_read32(USEC_TIMER_ADDR);
        if ((uint32_t)(nowh - play_down_us) > 5000000u &&
            (uint32_t)(nowh - dark_us)      > SUSPEND_ESCALATE_DARK_US) {
            if (enter_standby() != 0) {   /* true off (PMU) — normally no return */
                standby_refused = 1;
                was_playing     = 0;      /* stopped, not paused: nothing to resume */
                break;
            }
        }
        /* See the wake loop below for why this drain is load-bearing. */
        while (clickwheel_get_event(&drain)) { }
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }   /* drop the trigger's latched events */

    /* Low-power idle until any button is pressed. The (now 10 Hz) tick keeps
     * sampling the wheel into the latch through each cpu_wait. A press is
     * seen two ways: the live held-button state below, and any button
     * DOWN-EDGE the drain hands back — at 10 Hz a press can start and end
     * between two of the loop's own looks at the live state, but the tick
     * sampler latched its edge, and dropping that on the floor would be a
     * wake the user made and the device ignored. */
    /* Reached the idle loop: the last thing the log can say before the
     * device goes quiet, and the first thing a stuck suspend is missing.
     * Not flushed (the drive is asleep); it rides out in the wake's block.
     * The line un-gates SER0 to go out — re-gate it, or the suspend spends
     * the night with the UART clocked. */
    uart_puts("core: suspend: idle loop\n");
#if SUSPEND_GATE_CLOCKS
    if (g_suspend_lp) {
        uart_clock_suspend();
    }
#endif
    int pressed = 0;
    while (!standby_refused && !pressed && clickwheel_buttons() == 0) {
        /*
         * DRAINING HERE IS WHAT MAKES THE DEVICE WAKE AT ALL.
         *
         * clickwheel_buttons() returns a CACHED value that only the tick
         * sampler refreshes. When the Hold switch goes off, the sampler gates
         * the OPTO block's clock back on but deliberately defers the reset/
         * config sequence out of ISR context by setting s_need_bringup — and
         * then bails on every subsequent tick until something clears it. The
         * only thing that clears it is clickwheel_get_event().
         *
         * So a user who suspends and then flips Hold on and off again (the
         * standard reflex after "turning it off") left the cached button state
         * pinned at 0 forever: this loop span on a value that could never
         * change, dark and unresponsive, with a hardware reset the only escape.
         * The normal main loop never hit this because it drains every pass.
         *
         * Draining does not clear the cached state, so the loop condition is
         * unaffected — it only lets the deferred bring-up actually run.
         * (A Hold edge is an event with no buttons: not a wake.)
         */
        while (clickwheel_get_event(&drain)) {
            if (drain.buttons != 0) {
                pressed = 1;
            }
        }
        if (pressed) {
            break;                /* wake now, not after another 100 ms halt */
        }
        /*
         * Pump while suspended, so the codec actually powers down.
         *
         * The pause above stops the DMA but leaves the WM8758 fully powered —
         * PLL, VMID, DACs, headphone amps — and the thing that shuts it off is
         * the persistent-pause timeout inside player_pump(). This loop never
         * called pump, so a device "asleep" via Play-hold held the codec live
         * for the entire suspend, which is the most expensive way to do
         * nothing that this firmware is capable of.
         *
         * Safe to call here: pump's paused branch does the codec-off check and
         * returns immediately. No decode, no disk, no ring work — which
         * matters, because the drive is parked by this point and waking it
         * would defeat the whole exercise.
         */
        player_pump();
        /* Nothing above may hold a boost, but cpu_boost() from the crystal
         * un-parks the PLL and cpu_unboost() leaves it at 30 MHz — so a
         * future disk touch in this loop would silently spend the rest of
         * the suspend off the crystal. Re-park if that happened. */
#if SUSPEND_PARK_PLL
        if (g_suspend_lp && cpu_frequency() != CPUFREQ_DEFAULT) {
            (void)clock_suspend();
        }
#endif
        /* DEVICE 2026-09-13: the guard above used to run UNCONDITIONALLY —
         * so the PLL park executed on every suspend of every image that
         * evening, including the ones with SUSPEND_PARK_PLL 0, and the wake
         * side (which honours the switch) never un-parked it. Every "all
         * off" bisect step was therefore really "PLL park on". */

        /*
         * Watch the battery. battery_refresh() only ever ran from the main
         * loop, so a suspended device took no samples at all: it discharged
         * straight past the DISKSAFE and SHUTOFF lines to the PMU's hard cut,
         * with nothing flushed and no goodbye. Same call, same 5 s cadence,
         * same policy — the DISKSAFE edge makes its last write (which wakes
         * the slept drive, so put it back to sleep afterwards if the handler
         * left it up), and the SHUTOFF edge powers the device off through
         * enter_standby(). If the PMU refuses THAT, enter_standby has already
         * stopped the player and relit the screen: leave the loop the way a
         * refused hold-escalation does, and do not resume.
         *
         * The sample runs at the normal operating point: the clocks come
         * back for it and are parked again after. The gated blocks would
         * re-gate themselves, but the sample's consequences — a DISKSAFE
         * settings write on a boosted ATA transfer, a SHUTOFF power-off
         * through enter_standby — are paths calibrated at the PLL clocks,
         * and the log line goes out on the boot-tested UART setup. Two PLL
         * relocks per 5 s is a few hundred microseconds of duty.
         */
        if (battery_due()) {
            suspend_lowpower_leave();
            (void)battery_refresh(0);
            if (g_standby_refused) {
                standby_refused = 1;
                was_playing     = 0;
                break;
            }
#if SUSPEND_ATA_SLEEP
            if (!ata_is_slept()) {
                ata_sleep();        /* the handler re-parks with STANDBY only */
            }
#else
            if (!ata_is_parked()) {
                ata_standby();
            }
#endif
            suspend_lowpower_enter();
        }

        /*
         * Keep the jack watcher fed (one GPIO read through the debouncer), so
         * a plug pulled while the device sleeps is SEEN rather than inferred
         * from the level at wake. `was_playing` is the transport's state as
         * far as this feature is concerned — the pause above was the sleep's,
         * not the listener's — so a pull here answers PAUSE, and answering it
         * by dropping `was_playing` is how the wake below declines to resume.
         *
         * This is what makes "pulled and re-inserted while it slept" stay
         * paused: without it the wake sees a seated plug and cannot tell that
         * anything happened. Nothing is printed from inside the park (SER0's
         * clock may be gated); the wake says it once the clocks are back.
         */
        uint32_t now = mmio_read32(USEC_TIMER_ADDR);
        if (jackwatch_feed(&g_jack, hal_headphones_present(), was_playing,
                           now) == JACKWATCH_PAUSE) {
            was_playing = 0;
            jack_pulled = 1;
        }

        /*
         * Escalate. Past SUSPEND_TO_STANDBY_US on battery this is no longer
         * a short "off": trade the instant wake for a real power-down before
         * the cell is spent. Checked every pass, so a device unplugged after
         * the deadline escalates on the next one.
         */
        if (!power_is_external() &&
            (uint32_t)(now - suspend_t0) > SUSPEND_TO_STANDBY_US) {
            if (enter_standby() != 0) {   /* normally no return */
                standby_refused = 1;
                was_playing     = 0;
                break;
            }
        }
        cpu_wait_ms(SUSPEND_IDLE_MS);
    }
    /* Swallow the wake press so it isn't also acted on as navigation. */
    while (clickwheel_buttons() != 0) {
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }

    /* Back to the normal operating point FIRST — PLL at 30 MHz, 100 Hz tick,
     * SER0/PWM/I2C clocks restored — so the boost below is an ordinary
     * 30 -> 80 MHz switch and everything after it runs on the clocks it
     * was calibrated at. No-op if a refused standby already brought us
     * back, or if the park was never entered. */
    suspend_lowpower_leave();

    /* Re-boost BEFORE the drive and the repaint: cpu_boost/cpu_unboost are
     * refcounted, so this pairs with the unboost on the way in and keeps the
     * count balanced (an unmatched unboost would drive it negative the next
     * time the idle path unboosts). It also puts the ATA read and the render
     * back at 80 MHz, which is where their timing was calibrated. */
    cpu_boost();
    ata_wakeup();                         /* reset + spin the drive back up before any read */
    uart_puts(standby_refused ? "core: suspend: wake (standby refused)\n"
                              : "core: suspend: wake\n");
    /*
     * The clock, before anything paints one. The USEC_TIMER cannot be trusted
     * across a suspend — the PLL was parked and the tick dropped to 10 Hz — so
     * the software clock is RE-ANCHORED from the chip rather than carried over
     * a gap it has no way to measure. The RTC itself kept counting: it is in
     * the PMIC's always-on domain (docs/hw/06-power.md).
     */
    clock_resync("wake", 0);
    g_clock_min = 0;                      /* the strip repaints with the screen */
    /*
     * Panel back, in the only order that is safe: wake the panel driver,
     * render the real screen while everything is still dark, present it —
     * this present carries the BCM's ~500 ms panel init and does not return
     * until that has retired — and ONLY THEN light the backlight. Light any
     * earlier and the init shows as a white flash; on the old absorb
     * ordering it could latch white for good. lcd_wake() is a no-op when
     * the panel was not slept (SUSPEND_PANEL_SLEEP 0, or a refused standby
     * that already woke it), and the present is then the ordinary one.
     */
    lcd_wake();
    paint_current_screen();               /* render the real screen while dark... */
    lcd_present_fb(console_framebuffer()); /* ...retire the panel init...     */
    backlight_set(g_settings.backlight_bright);  /* ...then light up straight to it */
    /*
     * The jack, once, for both of the decisions below. The watcher was fed
     * through the whole suspend, so it has already accounted for anything
     * that happened; priming it here is the backstop for the paths that
     * leave that loop without a last feed (a refused standby, a battery
     * verdict), and it costs nothing when the level has not moved.
     */
    int hp = hal_headphones_present();
    jackwatch_prime(&g_jack, hp);
    if (jack_pulled) {
        /* Said here rather than inside the park: SER0's clock may be gated
         * down there. */
        uart_puts("core: jack out during suspend, staying paused\n");
    }
    if (was_playing) {
        /*
         * Resume only into a seated plug. -1 (detect not yet trusted on this
         * device, hal/hw/headphone.h) keeps today's behaviour: resume. 0
         * leaves it paused, one PLAY away.
         */
        if (hp != 0) {
            player_resume();
        }
    }
}
