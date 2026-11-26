/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Boot bring-up (proven on real 5.5G hardware): UART banner, 30 MHz clock,
 * unified cache, 100 Hz tick + IRQs, then — if the BCM LCD is powered — the
 * disk/audio stack. From there kernel_main mounts the FAT32 volume and runs the
 * menu UI: a main menu (Music / … / Now Playing), a Music sub-menu, and an
 * album/folder browser that streams the selected track through the background
 * player (core/player/player.c) and out the DMA-fed DAC. MENU pops screens; a
 * playing track keeps going while you navigate.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/ata.h"
#include "hw/clickwheel.h"
#include "hw/backlight.h"
#include "hw/battery.h"
#include "hw/headphone.h"
#include "hw/i2c.h"
#include "hw/power.h"
#include "hw/audio.h"
#include "hw/piezo.h"
#include "hw/rtc.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "cache.h"
#include "console.h"
#include "panic.h"
#include "config.h"
#include "resume_ctx.h"
#include "cfg_commit.h"
#include "evlog.h"
#include "chargestat.h"
#include "idlesleep.h"
#include "drivepower.h"
#include "backlightpolicy.h"
#include "otg_store.h"
#include "core_version.h"       /* CORE_BUILD_ID: meson vcs_tag, git describe   */
#include "core_version_tag.h"   /* CORE_VERSION:  meson vcs_tag, nearest tag    */
#include "../ui/text.h"
#include "../ui/thumb.h"
#include "../ui/artcache.h"
#include "../ui/screen_charging.h"
#include "../ui/screen_battery.h"
#include "../ui/settings.h"
#include "../ui/eq.h"
#include "../ui/palette.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "../library/idx.h"
#include "../library/sort.h"
#include "../library/playlist.h"
#include "../library/otg.h"
#include "../library/otg_slot.h"
#include "../ui/wheel.h"
#include "../ui/letterindex.h"
#include "../ui/search.h"
#include "../ui/keyhold.h"
#include "../ui/gesture.h"
#include "../ui/sleeptimer.h"
#include "../ui/jackwatch.h"
#include "../ui/settime.h"
#include "datetime.h"
#include "timesync.h"
#include "wallclock.h"
#include "hw/volume.h"
#include "../app/state.h"
#include "../app/layout.h"
#include "../app/util.h"
#include "../app/bootscreen.h"
#include "../app/marquee.h"
#include "../app/clockui.h"
#include "../app/persist.h"
#include "../app/nav.h"
#include "../app/library.h"
#include "../app/battery_ui.h"
#include "../app/playctx.h"
#include "../app/statusbar.h"
#include "../app/playlists.h"
#include "../app/albums.h"
#include "../app/nowplaying.h"
#include "../app/otg.h"
#include "../app/settings_glue.h"
#include "../app/lists.h"
#include "../app/search_glue.h"
#include "../app/resume.h"
#include "../app/paint.h"
#include "../app/az.h"
#include "../app/actions.h"
#include "../app/ui_loop.h"
#include "../app/main_internal.h"

/*
 * The Settings shuffle row and the player's playback order are the same three
 * values, deliberately: the byte in the config record is the enum, and
 * settings_apply() hands it straight to player_set_shuffle(). They are
 * declared in two headers because the player must not depend on the UI, so
 * this is the seam that holds them together.
 */
_Static_assert((int)SHUFFLE_OFF    == PLAYER_SHUFFLE_OFF,
               "settings shuffle ids must be the player's");
_Static_assert((int)SHUFFLE_SONGS  == PLAYER_SHUFFLE_SONGS,
               "settings shuffle ids must be the player's");
_Static_assert((int)SHUFFLE_ALBUMS == PLAYER_SHUFFLE_ALBUMS,
               "settings shuffle ids must be the player's");

/* Host-findable version stamp: `core info` scans the OSOS body for this tag
 * (see core/docs/design/companion-app-plan.md, S8). Never printed — not on the
 * UART (the clicky boot golden matches those lines exactly) and not on the
 * screen; the only reader is the host, which greps the image for
 * "CORE-FW-VERSION:". `used` stops the compiler dropping an unreferenced
 * definition; the LINK also needs boot/linker.ld's KEEP on this section,
 * because --gc-sections collects it otherwise (verified: without the KEEP the
 * string is absent from core.bin).
 * tests/scripts/check_version_marker.sh asserts exactly one copy
 * survives into build-hw/core.bin and that its version half is the repo's
 * nearest tag. CORE_BUILD_ID carries "-dirty" when the tree is dirty, which
 * is the point: an image built from an uncommitted tree says so. */
const char core_version_marker[] __attribute__((used, section(".rodata.core_version"))) =
    "CORE-FW-VERSION:" CORE_VERSION "|" CORE_BUILD_ID "\0";


/* ---------------------------------------------------------------------------
 * Design-matched list chrome (menus.jsx): status strip, header, rows, scrollbar
 * ------------------------------------------------------------------------- */

#define LOCK_FLASH_US 1000000u

/* Set by enter_standby() when the PMU refused the power-down and the device
 * was brought back instead: run_ui reads and clears it to resync its
 * backlight/idle state with the screen enter_standby already relit. */
static int g_standby_refused;

/* settings_commit() modes — CFG_COMMIT_IDLE / _SOFT / _FORCE / _LAST — and
 * the policy behind them (the DISKSAFE exemption in particular, which is not
 * optional, and the rule that only a FORCE ever wakes a parked drive) live in
 * cfg_commit.h with the gate that implements them. */

/*
 * The jack probe's narration. These lines go through the evlog tap into
 * CORELOG.BIN, so a bench session on a cable-less device leaves a written
 * record: plug, unplug, dump the log afterwards. Budgeted because the RAW
 * level is un-debounced and a cable chewed in a pocket would otherwise fill
 * the ring — the same "cannot spam" promise the UART probe makes. Debounced
 * edges are unbudgeted: at most one per genuine transition.
 */
#define JACK_RAW_LINE_BUDGET 64u

static unsigned g_jack_raw_lines;

static void jack_narrate_raw(int raw, unsigned edges)
{
    if (g_jack_raw_lines >= JACK_RAW_LINE_BUDGET) {
        return;
    }
    if (++g_jack_raw_lines == JACK_RAW_LINE_BUDGET) {
        uart_puts("core: jack raw line budget spent; silent from here\n");
        return;
    }
    uart_puts("core: jack raw=");
    uart_dec(raw);
    uart_puts(" n=");
    uart_dec((int)edges);
    uart_putc('\n');
}

/* The About screen's jack token carries the pin configuration, and ui/ is
 * host-built so it cannot see hal/hw/headphone.h. Two names for one bit is
 * fine as long as they cannot drift. */
_Static_assert(ABOUT_JACK_PIN_ENABLED == HEADPHONE_PIN_ENABLED &&
               ABOUT_JACK_PIN_OUTPUT  == HEADPHONE_PIN_OUTPUT,
               "ui/settings.h mirrors hal/hw/headphone.h's pin-config bits");

/*
 * The drive-power policy's state (kernel/drivepower.h: the idle spin-down,
 * the lock park, the browsing hold, and whether each park was worth it).
 */
drivepower_t g_drivepower;
/* Park accounting: the timer is read only on a park edge. */
static void disk_park_track(int playing)
{
    int parked = ata_is_parked();
    if (!drivepower_park_edge(&g_drivepower, parked)) return;
    drivepower_park_note(&g_drivepower, parked, playing,
                         mmio_read32(USEC_TIMER_ADDR));
}

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

static void suspend_to_ram(uint32_t play_down_us)
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

/*
 * Panel sleep at IDLE: when the backlight times fully off, put the LCD panel
 * to sleep too (LCD_SLEEP), not just the LED. The panel driver is the second
 * largest draw on a dark, playing device after the LED itself.
 *
 * HISTORY. The first attempt left the screen solid WHITE until a reboot — the
 * worst failure on a device whose only debug channel is that screen. The
 * mechanism (docs/hw/02-lcd.md): the first LCD_UPDATE after LCD_SLEEP makes
 * the BCM re-run its internal panel init, allowed up to 500 ms. At the time the
 * commit handshake budgeted ~2 ms and RE-KICKED LCD_UPDATE 16 times inside that
 * window, every later present streamed a fresh 150 KB frame into the
 * in-progress init, and the backlight was lit at the input site before any of
 * it — so the init never completed and the BCM latched. The doc names the
 * symptom: "If we wake the backlight before the first update completes, the
 * user sees a 500 ms white flash."
 *
 * WHAT IS TRUE NOW, and why this is on:
 *   - hal/hw/lcd.c bcm_frame_commit: the first present after lcd_wake()
 *     issues its LCD_UPDATE and then WAITS, wall-clock bounded and with NO
 *     re-kick, until that update has retired. The present returns with the
 *     panel init done (hw-lcd-present: test_lcd_present_post_wake).
 *   - While slept, every present is REFUSED and counted (lcd_presents_refused;
 *     test_lcd_present_refused_reported), so nothing can stream into the
 *     sleeping BCM or its wake-init. In this loop every painter — the render
 *     block, the two bar animators, the marquee, the lock plate, the toast —
 *     is gated on bl_state != BL_OFF anyway, so on the idle path nothing even
 *     tries; the count is the proof of that on the device.
 *   - The wake is ONE block (search "Panel wake"), in the only order that is
 *     safe and the same one suspend_to_ram and enter_standby use: lcd_wake();
 *     paint the current screen while everything is still dark; present that
 *     FULL frame (it absorbs the init); THEN backlight_set(). The input sites
 *     do not light the LED themselves when the panel is slept — they only
 *     move bl_state, and the wake block does the rest before anything else in
 *     the pass can present. The wake frame is full by construction, so a
 *     partial-paint cache (g_lp, np_last) that went stale while dark cannot
 *     put a two-row partial on top of pixels the BCM no longer has.
 *   - Two owners compose: a panel slept here and then suspended (hold PLAY
 *     from dark) is slept once — lcd_sleep is idempotent — and woken once by
 *     suspend's own wake; the loop clears panel_slept on the way back so it
 *     does not wake twice. Likewise a refused PMU standby.
 *   - lcd_recover() (a real bcm_init) exists for a wake that never retires,
 *     but stays compiled out (LCD_RECOVER_ON_WAKE 0) until it has run on
 *     silicon; a failed absorb is reported (lcd_bcm_timeouts) and the frame
 *     goes out anyway, which is what the old code did on every frame.
 *
 * UNVERIFIED ON THE DEVICE as of 2026-09-13; suspend's panel sleep
 * (SUSPEND_PANEL_SLEEP) is the same lcd_sleep/lcd_wake pair and is equally
 * unflashed. First-flash check: let the backlight time out, wait 10 s, press
 * a button — the screen must come back with the right content, not white.
 * ROLLBACK: set this to 0. The LED still times off exactly as before; only the
 * LCD_SLEEP at the BL_OFF edge and the wake block's lcd_wake go away (the
 * wake block is then a no-op, since panel_slept never sets).
 */
/* DEVICE RESULT 2026-09-13: WHITE on the first wake (backlight timed out
 * while playing, ~1 min dark, Menu → solid white). Rolled back to 0 as the
 * note above says. What the bench still needs: whether the white is the
 * first present racing the panel init (then the absorb window is wrong)
 * or LCD_SLEEP itself needing a different wake command on this BCM. */
#ifndef PANEL_SLEEP_AT_IDLE
#define PANEL_SLEEP_AT_IDLE  0
#endif

/*
 * run_ui's prologue: everything before the event loop, in boot order (the
 * order is load-bearing; the comments below say why at each step), ending
 * with the loop state *L given its initial values.
 *
 * Forced inline: left to itself GCC keeps this as a call and inlines the
 * boot-only callees (config_load, evlog_mount, otg_store_mount,
 * resume_restore) into it, a 456 B frame stacked on run_ui's for the boot's
 * library load, +280 B on the deepest call chain. Inlined into run_ui, the
 * frames and the chain are what they were before the split.
 */
__attribute__((always_inline)) static inline void ui_boot(fat32_t *fs, ui_loop_t *L)
{
    clickwheel_init();
    player_init(fs);
    /* The charger's budget, from the setting (defaults until config_load
     * below, then settings_apply() re-applies the saved choice). Done this
     * early so a device booted on the cable draws the budget from the first
     * second: at the LTC4066's 100 mA default the boot-time library load and
     * spin-ups run off the cell even with the cable in. DEVICE 2026-09-13:
     * on USB the jack carried the drive and the piezo (a cable ground loop)
     * and a whine, and 100 mA was quieter — that is what Charge Rate = 100 mA
     * is for. DEVICE 2026-09-19 (event log): at 100 mA the cell did not
     * climb while the device was used at all. */
    charger_apply();
    artcache_init();                  /* ways must start at key -1; .bss gives 0,
                                       * which is album 0's real index */
    /*
     * Settings: defaults FIRST, then let the saved record overwrite them.
     * config_load() only touches g_settings when it finds a record whose
     * magic, version and CRC all check out, so any failure — file absent,
     * torn write, corrupt slot, unresolvable cluster — leaves the full
     * default set in place. Defaults are the floor, never skipped.
     *
     * Read BEFORE the library (it needs only `fs`, and the drive is spinning
     * right after the mount either way) so the saved theme is known before
     * anything paints a loading screen. Boot Details' OTHER bucket — total
     * minus the named phases — absorbs this read either way; g_lib_load_ms is
     * measured inside library_ensure and is unaffected.
     */
    int clock_mark_unsaved = 0;           /* see the timesync block below */
    settings_defaults(&g_settings);
    int cfg_ok = config_load(fs, &g_settings);
    uart_puts("core: cfg load ");
    uart_put_hex32((uint32_t)cfg_ok);
    uart_puts(" writable ");
    uart_put_hex32((uint32_t)config_writable());
    uart_puts(" seq ");
    uart_put_hex32(config_seq());
    /* The address a save WOULD target, printed before any write can happen.
     * The first on-device bring-up must check this against the LBA
     * tools/make_config.py --verify computes on the host (see the procedure
     * at the top of kernel/config.c) — a mismatch here is the failure mode
     * that destroys a music library. */
    uint32_t cfg_lba0 = 0, cfg_lba1 = 0;
    (void)config_probe_lba(0, &cfg_lba0);
    (void)config_probe_lba(1, &cfg_lba1);
    uart_puts(" lba ");
    uart_put_hex32(cfg_lba0);
    uart_putc('/');
    uart_put_hex32(cfg_lba1);
    uart_putc('\n');
    g_diag_cfg_lba[0] = cfg_lba0;         /* Boot Details, without a spin-up      */
    g_diag_cfg_lba[1] = cfg_lba1;

    /*
     * THE CLOCK, decided once, here, while the drive is still spinning from
     * the mount and before anything else can go wrong.
     *
     * The host cannot tell us the time — on the cable it is Apple's ROM disk
     * mode answering, not us — so `core sync` / `core eject` / `core install`
     * leave a stamp in the record we just loaded, and this is where it is
     * judged. kernel/timesync.c owns the rules (one action per stamp, and a
     * stamp the RTC has already run ten minutes past is inert); main.c owns
     * only the wiring: read the chip, ask, act, record.
     *
     * The mark is FORCE-committed immediately rather than left to the debounce
     * because the boot's own cfg_commit_clear() is a few lines below, and a
     * mark that never reached the platter would let the same stamp be applied
     * again on the next boot.
     */
    {
        /* ONE pass over the chip: the raw bytes are logged and the decision is
         * made from exactly those bytes. Reading twice would cost three more
         * I2C transactions and, worse, let the log line and the decision
         * disagree — and that line is the only evidence the register map is
         * right at all (docs/hw/06-power.md, "Bench procedure"). */
        uint32_t   now_us = mmio_read32(USEC_TIMER_ADDR);
        uint8_t    raw[RTC_REG_COUNT];
        datetime_t rtc_civil;
        uint32_t   rtc_epoch = 0;
        int        rtc_rc    = -1;            /* the bus did not answer */

        if (rtc_read_raw(raw) == 0) {
            uart_puts("core: rtc raw");
            for (int i = 0; i < RTC_REG_COUNT; i++) {
                uart_putc(' ');
                uart_put_hex32(raw[i]);
            }
            rtc_rc = rtc_decode(raw, &rtc_civil);
            if (rtc_rc == 1) {
                rtc_epoch = datetime_to_epoch(&rtc_civil);
            }
            uart_puts(" valid ");
            uart_put_hex32((uint32_t)rtc_rc);
            uart_puts(" epoch ");
            uart_put_hex32(rtc_epoch);
            uart_putc('\n');
        }

        wallclock_anchor(&g_wclock, rtc_rc == 1, rtc_epoch, now_us);
        g_clock_tick_us   = now_us;
        g_clock_resync_us = now_us;
        g_clock_retry     = (rtc_rc < 0);   /* no answer: ask again in a minute */

        timesync_state_t ts = {
            g_settings.host_epoch, g_settings.host_off_min,
            g_settings.applied_epoch, g_settings.utc_off_min,
            timesync_host_dst(g_settings.host_epoch,
                              (unsigned)g_settings.host_dst_raw),
        };
        timesync_action_t act = timesync_decide(&ts, config_writable(),
                                                rtc_rc == 1, rtc_epoch);
        int set_rc = 0;
        if (act == TIMESYNC_SET) {
            set_rc = hal_rtc_set(ts.host_epoch);
            if (set_rc == 0) {
                wallclock_anchor(&g_wclock, 1, ts.host_epoch, now_us);
            } else {
                /* The write did not take. Leave the mark alone so the next
                 * boot retries the same stamp; rule 5 bounds how far back
                 * that can ever pull the clock. */
                act = TIMESYNC_NONE;
            }
        }
        if (timesync_apply(&ts, act)) {
            g_settings.applied_epoch = ts.applied_epoch;
            g_settings.utc_off_min   = ts.utc_off_min;
            settings_touch();
            settings_commit(CFG_COMMIT_FORCE);
            /*
             * THIS WRITE IS PRE-POLICY, deliberately. battery_disk_writes_
             * allowed() answers from `bat_level`, and the policy cannot move
             * that until its median ring holds BATTERY_FILTER_N samples — five
             * of them, 5 s apart, so roughly 20 s into a boot that is still
             * spinning the drive up (hal/hw/battery.c). Sampling the cell here
             * would not change that: five conversions microseconds apart are
             * not a median over time, they are one spin-up-sagged reading with
             * a quorum, which is the decision the filter exists to refuse.
             *
             * That is acceptable for exactly this write: the platters are
             * already up from the mount (the index load follows), it is one sector,
             * and SHUTOFF cannot have fired yet (nothing has been able to
             * judge the cell). Every later save meets the armed gate.
             *
             * What CAN still happen is a refusal for another reason, or a
             * failed write — and either leaves the change PENDING, which
             * cfg_commit_clear() at the end of this boot would throw away with
             * the load's own dirt. Carry the fact forward so the mark is
             * re-armed instead of lost; a lost mark means this stamp is
             * applied again on the next boot, and on the one after that, for
             * as long as the writes keep failing.
             */
            clock_mark_unsaved = g_cfg_commit.dirty;
        }

        uart_puts("core: timesync host ");
        uart_put_hex32(g_settings.host_epoch);
        uart_puts(" off ");
        uart_put_hex32((uint32_t)(int32_t)g_settings.host_off_min);
        uart_puts(" applied ");
        uart_put_hex32(g_settings.applied_epoch);
        uart_puts(" rtc ");
        uart_put_hex32(rtc_epoch);
        uart_puts(act == TIMESYNC_SET   ? " -> set"   :
                  act == TIMESYNC_STALE ? " -> stale" : " -> none");
        uart_puts(" rc ");
        uart_put_hex32((uint32_t)set_rc);
        uart_putc('\n');
    }

    /* The saved theme is known now. Apply it BEFORE the library load so the
     * loading screen (and the album-chip placeholders below) come up in the
     * user's theme; settings_apply() later re-applies it harmlessly along with
     * the audio settings, which want the codec path settled first. */
    theme_set(g_settings.theme);
    boot_screen_render("LOADING", -1);    /* the same screen, now themed          */

    chip_placeholder_init();              /* after theme_set: it bakes
                                           * LINEN_BORDER in at call time
                                           * (settings_apply re-bakes it on a
                                           * live theme change) */
    library_ensure(fs);                   /* preload the index at boot (drive is
                                           * spinning) so Songs/Albums/Artists/
                                           * Genres open INSTANTLY, like Apple —
                                           * not a multi-second stall on first use */
    battery_refresh(1);                   /* prime the status-strip gauge         */
    g_volume = hal_volume_get();          /* reflect the codec's default gain      */
    g_dir_depth = 0;
    g_browse_n  = 0;
    g_br_sel = g_br_accum = 0;
    g_main_sel  = 0;
    g_music_sel = MU_ALBUMS;
    g_menu_accum = 0;
    g_artist_filter[0] = '\0';
    g_artist_sel = g_artist_accum = 0;

    /*
     * The event log: locate CORELOG.BIN, validate its header, find the
     * write cursor. Everything narrated so far this boot is already in the
     * capture ring and goes out in the first block. The line below is the
     * log's own first-flash gate: the two LBAs must equal what
     * tools/make_log.py --verify computed on the host (see kernel/evlog.h
     * and the procedure at the top of kernel/config.c, which this module
     * owes as the write path's second caller). `prev` is the boot reason as
     * far as the log knows it — whether the previous session's last block
     * was a forced (final) flush, or the session just stopped.
     */
    int ev_on = evlog_mount(fs, ata_write_sectors, ata_wakeup);
    uart_puts(ev_on ? "core: evlog on seq " : "core: evlog off seq ");
    uart_put_hex32(evlog_seq());
    uart_puts(" boot ");
    uart_put_hex32(evlog_boot_id());
    uart_puts(evlog_prev_final() < 0 ? " prev none" :
              evlog_prev_final()     ? " prev final" : " prev unflushed");
    uint32_t ev_lba0 = 0, ev_lban = 0;
    (void)evlog_probe_header_lba(&ev_lba0);
    (void)evlog_probe_lba(evlog_seq(), &ev_lban);
    uart_puts(" lba ");
    uart_put_hex32(ev_lba0);
    uart_putc('/');
    uart_put_hex32(ev_lban);
    uart_putc('\n');
    g_diag_log_lba[0] = ev_lba0;          /* Boot Details, without a spin-up      */
    g_diag_log_lba[1] = ev_lban;

    /*
     * The On-The-Go live list: COREOTG.DAT's newest slot into g_otg, then the
     * locator pairs bound to songs (the library is loaded by now). The two
     * LBAs are this module's own first-flash gate and mean exactly what
     * config.c's and evlog.c's do — they MUST equal what
     * tools/make_otg.py --verify computed on the host, checked BEFORE the
     * first add, or the write that follows lands somewhere else on the disk.
     */
    int otg_ok = otg_store_mount(fs, ata_write_sectors, ata_wakeup, &g_otg);
    otg_bind_all();
    uart_puts("core: otg load ");
    uart_dec((int)g_otg.n);
    uart_puts(" writable ");
    uart_put_hex32((uint32_t)otg_store_writable());
    uart_puts(" seq ");
    uart_put_hex32(otg_store_seq());
    uint32_t otg_lba0 = 0, otg_lba1 = 0;
    (void)otg_store_probe_lba(0, &otg_lba0);
    (void)otg_store_probe_lba(1, &otg_lba1);
    uart_puts(" lba ");
    uart_put_hex32(otg_lba0);
    uart_putc('/');
    uart_put_hex32(otg_lba1);
    uart_putc('\n');
    (void)otg_ok;                         /* the count above already says it */

    settings_apply();                     /* push shuffle/repeat/volume out       */
    /* Pick up where the user left off — after settings_apply (it needs the
     * saved volume, which the restore mutes across the open and puts back) and
     * after library_ensure (the locator is resolved against the library). Comes
     * up PAUSED; nothing here starts audio. A no-op when Resume is off, when
     * nothing was saved, or when the saved track can't be resolved. */
    uint32_t resume_t0 = boot_ms_now();
    resume_restore(fs);
    g_boot_resume_ms   = boot_ms_now() - resume_t0;
    /* Everything the user waits for is now done; the next thing that happens is
     * the first UI paint. Stamped here rather than after the present so the
     * number means "time until the device was ready", not "+1 frame". */
    g_boot_total_ms    = boot_ms_now();
    cfg_commit_clear(&g_cfg_commit);      /* loading is not a change to save back */
    otg_store_commit_clear();             /* ...nor is loading the On-The-Go list */
    if (clock_mark_unsaved) {
        /* ...but the clock's mark IS a change, and it did not reach the platter
         * above. Re-arm it for the idle path rather than let the line above
         * discard it (kernel/timesync.h: a stamp acted on but not marked is
         * acted on again next boot). */
        settings_touch();
        uart_puts("core: timesync mark not saved yet — retrying\n");
    }
    g_scr_n = 0;
    scr_push(SCR_MENU);

    L->dirty = 1;
    L->np_last = 0xFFFFFFFFu;
    L->np_first = 1;
    L->np_vol_prev = 0;            /* volume overlay was up last NP paint  */
    L->np_vol_dirty = 0;           /* g_volume moved since the plate paint */
    L->last_present = 0;           /* rate-limit UI presents while playing */
    L->last_bars = 0;              /* rate-limit the now-playing bar anim  */
    L->last_chip = 0;              /* rate-limit album-cover chip loads    */
    L->last_uistat = 0;            /* 5 s cadence of the "core: ui" line   */
    L->last_mq = 0;                /* rate-limit the marquee scroll        */
    L->hold_prev = clickwheel_hold() ? 1 : 0;  /* seed hold-edge detect    */
    L->ext_prev = power_is_external() ? 1 : 0; /* seed plug-in edge detect */
    L->banner_up = 0;          /* a top-chrome banner is on screen     */
    L->az_prev = 0;                /* A-Z locator letter on screen         */
    L->toast_prev = 0;             /* low-battery toast on screen          */
    L->bat_glyph_prev = battery_glyph_key(g_bat_pct); /* strip gauge as drawn */
    keyhold_reset(&L->play_key);            /* (g_ff/g_rw are statics: born idle)    */
    keyhold_reset(&L->menu_key);
    keyhold_reset(&L->row_key);
    g_rowsel.pending = 0;
    /* Nothing believed about the jack yet. The first pass primes it from the
     * HAL's own first (already primed) sample, so a boot with an empty jack
     * yields "out" with no edge and no pause. */
    jackwatch_reset(&g_jack);
    g_locked = L->hold_prev;

    /* Backlight inactivity: full -> dim -> off. Any input wakes to full; a press
     * that wakes from fully-OFF is swallowed (it just lights the screen, the way
     * a real iPod's first touch does). Playback keeps running the whole time. */
    L->bl_state = BL_FULL;
    L->cpu_idled = 0;              /* core dropped to 30 MHz for deep idle   */
    /* LCD panel put to sleep at backlight-off (PANEL_SLEEP_AT_IDLE). While set,
     * the input sites must NOT light the LED: the "Panel wake" block below
     * wakes the panel, paints, presents the full frame (which retires the
     * BCM's panel init) and only then lights it — see the note above run_ui. */
    L->panel_slept = 0;
    L->panel_refused_at_sleep = 0;  /* lcd_presents_refused() at the sleep */
    L->last_input = mmio_read32(USEC_TIMER_ADDR);
    idlesleep_reset(&g_idlesleep, L->last_input);   /* two minutes from here, untouched,
                                                  * and a fresh boot sleeps too — a
                                                  * cold boot from a dead cell used
                                                  * to sit awake until dead again */

    /* Seeded from the LIVE transport, not from zero: a successful resume_restore
     * has already left a track loaded, and a `was_active` of 0 would read the
     * first pass as a play->stop edge (closing the codec under a track we just
     * restored) and as a fresh capture of a position we only just loaded. */
    L->was_active = player_active();     /* detect the active->idle edge          */
    L->last_qidx = L->was_active ? player_queue_current() : -1;
    /* Which track is open, for the seek-aim cancel below. Keyed on the OPEN
     * and not on last_qidx because repeat-one re-opens the same queue index. */
    L->last_oseq = player_open_seq();
    g_resume_open_seq   = player_open_seq();
    g_resume_was_paused = player_paused();
    /* Seeded AFTER resume_restore: a restore that had to skip a broken track
     * bumps the end counter, and that must not read as "the queue finished". */
    g_resume_end_seq    = player_end_seq();
    g_resume_last_us    = mmio_read32(USEC_TIMER_ADDR);
}

/*
 * The UI: one event loop that pumps the background player every pass and
 * dispatches input to the current screen (Main menu / Music menu / Browser /
 * Now Playing) on a stack. MENU pops the screen WITHOUT stopping playback, so a
 * song keeps going while you navigate. Never returns.
 */
_Noreturn static void run_ui(fat32_t *fs)
{
    ui_loop_t  loop;                    /* the loop's state (app/ui_loop.h) */
    ui_loop_t *L = &loop;
    ui_boot(fs, L);
    for (;;) {
        /* Time the pump: when it returns in a few microseconds the decode step
         * produced nothing (PCM ring full) and the disk buffer isn't filling —
         * i.e. this pass had no work. Used at the bottom of the loop to halt. */
        uint32_t pump_t0 = mmio_read32(USEC_TIMER_ADDR);
        player_pump();
        uint32_t pump_us = mmio_read32(USEC_TIMER_ADDR) - pump_t0;

        /* TRACK-CHANGE EDGE — unconditional, above every screen-specific branch.
         * Auto-advance moves the queue with no button event, and several screens
         * paint the playing track (the status strip's name on ALL of them, the
         * tracklist's animated bars, the queue view's bars, the main menu's "Now
         * Playing" row). The bar animators are partial painters that only ever
         * repaint the row they find playing, so without a full repaint here the
         * FINISHED track's row keeps a frozen 3-bar glyph forever (one more each
         * time a track ends) and the status strip keeps its name. Covers the
         * play->stop transition too. */
        int now_active = player_active();
        int now_qidx   = now_active ? player_queue_current() : -1;
        if (now_active != L->was_active || now_qidx != L->last_qidx) {
            L->dirty = 1;
        }
        L->last_qidx = now_qidx;

        /* A RIGHT/LEFT aim belongs to the track it was aimed at, and THIS is
         * the only place that can see that track go. ui/gesture.c is fed a
         * position and a length, and the next track has both, so an aim that
         * survived an auto-advance would go on stepping and then commit a
         * position measured against a track nobody is playing any more — a
         * rewind held through the end of a 4:00 track landing 3:23 into the
         * next one, which the listener has heard none of. Keyed on
         * player_open_seq(), which bumps on every track open (auto-advance,
         * the gapless hand-over, a skip, a queue-view jump and repeat-one's
         * re-open of the SAME index) and not on a seek, so our own committed
         * seek does not look like a track change. The repaint is the dirty
         * above; where it is the queue ENDING, `allowed` drops and the feed
         * would cancel anyway — this just gets there first. */
        uint32_t now_oseq = player_open_seq();
        if (now_oseq != L->last_oseq || now_active != L->was_active) {
            L->last_oseq = now_oseq;
            (void)seekhold_cancel(&g_ff);
            (void)seekhold_cancel(&g_rw);
        }

        /* RESUME POSITION — the edges worth a kilobyte of disk (see the block
         * comment above resume_capture): the track changed, pause flipped,
         * playback started/stopped, or RESUME_SAMPLE_US has gone by. The
         * timestamp is stamped HERE and not inside resume_capture, so a pass
         * where nothing had moved can't re-arm the interval every iteration. */
        {
            uint32_t oseq  = player_open_seq();
            int      paus  = player_paused();
            uint32_t nowu  = mmio_read32(USEC_TIMER_ADDR);
            if (now_active != L->was_active || oseq != g_resume_open_seq ||
                paus != g_resume_was_paused ||
                (uint32_t)(nowu - g_resume_last_us) >= RESUME_SAMPLE_US) {
                resume_capture();
                g_resume_open_seq   = oseq;
                g_resume_was_paused = paus;
                g_resume_last_us    = nowu;
            }
        }

        /* Queue finished (last track of the album/playlist/song list ended and
         * Repeat is off): no screen may keep showing a dead player. Truncate the
         * stack at the FIRST player view (Now Playing / Queue — the queue view is
         * just as dead), keeping everything the user navigated through below it;
         * an empty stack falls back to the main menu. Testing only the TOP screen
         * left a dead Now Playing behind a Queue view, and left every other screen
         * un-repainted. */
        if (L->was_active && !now_active) {
            /*
             * If the queue ended BY ITSELF, drop the saved resume position.
             * resume_capture() deliberately keeps the last locator when
             * nothing is loaded ("nothing loaded — keep the last one"), which
             * is right for a stop but wrong here: the listener heard the track
             * to the end, so restoring it on the next boot re-presents a
             * finished song cued at 0:00 (device, 2026-07-27).
             *
             * Gated on player_end_seq() rather than on this transition alone,
             * so a manual stop still keeps its position.
             */
            uint32_t eseq = player_end_seq();
            if (eseq != g_resume_end_seq) {
                g_resume_end_seq = eseq;
                if (resume_ctx_clear(&g_settings)) {
                    settings_touch();  /* locator AND queue context, together */
                }
            }
            /* Playback truly ended (stop / queue exhausted / skip past the ends —
             * NOT an inter-track advance, which never reads inactive here since
             * advance+open complete inside one player_pump). Power the codec down
             * and gate the audio clocks; the next track's hal_audio_init does a
             * full pop-suppressed bring-up, so resume is clean. */
            hal_audio_close();
            for (int i = 0; i < g_scr_n; i++) {
                if (g_scr[i] == SCR_NOWPLAYING || g_scr[i] == SCR_QUEUE) {
                    g_scr_n = i;
                    break;
                }
            }
            if (g_scr_n == 0) {
                g_scr[0]   = SCR_MENU;
                g_scr_n    = 1;
                g_main_sel = 0;
            }
            L->dirty = 1;
        }
        L->was_active = now_active;
        if (battery_refresh(0)) {
            /* The strip's gauge is only ever painted on `dirty`, and an idle
             * list screen sets it for nothing, so a menu left alone showed
             * the boot-time gauge indefinitely. Repaint when the glyph as
             * DRAWN changes — at most once per 5 s sample, and not at all
             * for a sample that rounds to the same picture. The charging
             * screen shows the number itself, so every sample is a change. */
            int key = battery_glyph_key(g_bat_pct);
            if (key != L->bat_glyph_prev || scr_cur() == SCR_CHARGING ||
                (scr_cur() == SCR_SETTINGS && g_set_screen == SETTINGS_BATTERY)) {
                L->bat_glyph_prev = key;      /* the Battery page IS the sample */
                L->dirty = 1;
            }
        }

        /*
         * The clock. Three things, all of them cheap, none of them per frame:
         *
         *   - fold the elapsed microseconds into the software clock every 5 s.
         *     The USEC_TIMER wraps every ~71.6 minutes and an unsigned delta
         *     cannot tell one wrap from none (kernel/wallclock.h);
         *   - re-anchor from the chip every half hour — three I2C transactions
         *     an hour, and the only thing that corrects the timer's drift;
         *   - repaint on the MINUTE EDGE, and only where a clock is drawn.
         *     One comparison per pass, and then an ordinary `dirty` — a full
         *     paint and present, once a minute, on a screen that is idle by
         *     definition (a playing device repaints far more often than that).
         *     A band-only present would be the cheaper thing to reach for, but
         *     the clock sits in the header on the main menu and in a list row
         *     under Settings, neither of which is the strip's band.
         */
        {
            uint32_t now_us = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(now_us - g_clock_tick_us) >= CLOCK_TICK_US) {
                wallclock_tick(&g_wclock, now_us);
                g_clock_tick_us = now_us;
            }
            uint32_t due = (g_clock_retry ? RTC_RETRY_S : RTC_RESYNC_S) * 1000000u;
            if ((uint32_t)(now_us - g_clock_resync_us) >= due) {
                clock_resync("resync", 1);
            }
            uint32_t minute = 0;
            if (wallclock_minute(&g_wclock, now_us, &minute) &&
                minute != g_clock_min) {
                g_clock_min = minute;
                /*
                 * Only screens that actually show a clock. Date & Time's first
                 * row shows the time it would edit WHATEVER Time in Title says
                 * — that row is how you read the clock with the setting off —
                 * so it is outside the flag's test; the strip (idle) and the
                 * main menu's header are inside it.
                 */
                int on_dt = (scr_cur() == SCR_SETTINGS &&
                             g_set_screen == SETTINGS_DATETIME);
                int in_title = g_settings.time_in_title &&
                               (!player_active() || scr_cur() == SCR_MENU);
                if (on_dt || in_title) {
                    L->dirty = 1;
                }
            }
        }

        /*
         * The headphone jack. All of the policy is in ui/jackwatch.c; this is
         * sample, act, narrate.
         *
         * Sitting here, outside the g_locked branch and above the charging
         * modal, is deliberate: a yank in the pocket is the canonical case,
         * and neither Hold nor a modal may swallow it. The pause needs no
         * resume-position call of its own — the capture above fires on any
         * pause flip.
         *
         * The RAW level is read in every build, trusted or not: one 32-bit
         * read of the register the hold switch is already read from, and it
         * is the whole on-screen probe (hal/hw/headphone.h, "WHICH PROBE").
         */
        {
            int raw = headphone_raw();
            if (jackwatch_note_raw(&g_jack, raw)) {
                jack_narrate_raw(raw, g_jack.raw_edges);
                if (scr_cur() == SCR_SETTINGS && g_set_screen == SETTINGS_ABOUT) {
                    L->dirty = 1;          /* the digit follows the plug */
                }
            }
            int playing = player_active() && !player_paused();
            switch (jackwatch_feed(&g_jack, hal_headphones_present(), playing,
                                   pump_t0)) {
            case JACKWATCH_PAUSE:
                player_pause();
                L->dirty = 1;              /* the band must read Paused, like a PLAY tap */
                uart_puts("core: jack out, pause\n");
                break;
            case JACKWATCH_OUT:
                uart_puts("core: jack out\n");
                break;
            case JACKWATCH_IN:
                uart_puts("core: jack in\n");
                break;
            case JACKWATCH_NONE:
                break;
            }
        }

        /* Charging screen: pop up on a plug-IN edge (not if already powered at
         * boot, so it won't spuriously appear), auto-dismiss when unplugged. Any
         * button also dismisses it (handled in the input switch below). */
        int ext = power_is_external() ? 1 : 0;
        if (ext && !L->ext_prev && scr_cur() != SCR_CHARGING) {
            scr_push_modal(SCR_CHARGING);
            L->dirty = 1;
        } else if (!ext && scr_cur() == SCR_CHARGING) {
            scr_pop();
            L->dirty = 1;
        }
        L->ext_prev = ext;

        /* The low-battery modal. Raised once per OK->DISKSAFE crossing and
         * held until a button dismisses it; the charging screen outranks it,
         * because a plugged-in device has already answered the warning. This
         * is idempotent per pass, so it self-heals if anything else truncates
         * the screen stack underneath it. */
        if (battwarn_screen() == BATTWARN_DISKSAFE &&
            scr_cur() != SCR_BATTERY && scr_cur() != SCR_CHARGING) {
            scr_push_modal(SCR_BATTERY);
            L->dirty = 1;
        } else if (scr_cur() == SCR_BATTERY &&
                   battwarn_screen() != BATTWARN_DISKSAFE) {
            scr_pop();
            L->dirty = 1;
        }

        /*
         * PLAY press-length arbitration: a tap toggles pause, a hold of
         * PLAY_HOLD_US sleeps the device (suspend: drive spun down, screen
         * dark, but CPU+RAM alive so wake is INSTANT). Holding on to ~5 s
         * escalates to a true PMU power-down. Decided from LIVE button state
         * once per pass, by the same rule SELECT uses on Now Playing: the
         * down-edge decides nothing, the release before the threshold is the
         * tap, the first pass at the threshold is the hold. The pause toggle
         * used to fire on the down-edge EVENT regardless, so hold-to-sleep
         * paused first (and wake did not resume) and hold-while-paused played
         * for two seconds before sleeping. Locked out while the hold switch is
         * on; a press that woke the backlight or dismissed a modal has its tap
         * swallowed below (keyhold_swallow_tap) but can still hold.
         */
        {
            uint32_t nowp = mmio_read32(USEC_TIMER_ADDR);
            int      down = !g_locked &&
                            (clickwheel_buttons() & WHEEL_BTN_PLAY) != 0;
            /* Two things can put the device to sleep from here, and they must
             * not each carry their own copy of the bookkeeping: they set the
             * flag and the origin stamp, and ONE block below does the sleep. */
            int      want_suspend   = 0;
            uint32_t suspend_origin = nowp;

            switch (keyhold_feed(&L->play_key, down, nowp, PLAY_HOLD_US)) {
            case KEYHOLD_TAP:
                /* On a row that names music, PLAY plays it (play_tap_start);
                 * everywhere else it is the transport toggle, from any screen,
                 * like a real iPod. */
                switch (play_tap_start(fs)) {
                case PLAY_TAP_STARTED:
                    L->np_first = 1;
                    L->dirty    = 1;
                    break;
                case PLAY_TAP_CONSUMED:
                    L->dirty = 1;           /* started nothing, but not a toggle   */
                    break;
                case PLAY_TAP_PASS:
                    if (player_active()) {
                        player_toggle_pause();
                        L->dirty = 1;
                    }
                    break;
                }
                break;
            case KEYHOLD_HOLD:
                /* Keep timing the escalation to PMU standby from the moment
                 * the finger went down, not from now. */
                want_suspend   = 1;
                suspend_origin = keyhold_down_us(&L->play_key);
                break;
            case KEYHOLD_NONE:
                break;
            }

            /*
             * The sleep timer, fed from the same stamp. It runs regardless of
             * what the player is doing and regardless of the Hold switch — a
             * locked, pocketed device is the case the feature is FOR — and it
             * is not reset by input: it is a duration, not an idle timeout.
             */
            switch (sleeptimer_feed(&g_sleep, nowp)) {
            case SLEEPTIMER_TICK:
                L->dirty = 1;                      /* the token's minute changed */
                break;
            case SLEEPTIMER_FIRE:
                uart_puts("core: sleep timer: expired, sleeping\n");
                /* Pause BEFORE suspending rather than teaching suspend_to_ram
                 * a new argument: with the player already paused its
                 * `was_playing` is 0, so the wake comes back PAUSED — you fell
                 * asleep, and the next press is "where was I", not "play" —
                 * and the resume_capture() inside records the paused position.
                 */
                if (player_active() && !player_paused()) {
                    player_pause();
                }
                /* If a PLAY hold reached its threshold on this same pass, its
                 * down-edge is the EARLIER stamp and the one the 5 s
                 * escalation is documented to be timed from; do not push it
                 * forward to now. */
                if (!want_suspend) {
                    suspend_origin = nowp;
                }
                want_suspend = 1;
                break;
            case SLEEPTIMER_NONE:
                break;
            }

            /*
             * IDLE SLEEP (kernel/idlesleep.h). Two minutes with nothing
             * playing and no input, on battery, with no sleep timer running,
             * and the device sleeps the way a PLAY hold sleeps it: the same
             * suspend_to_ram, so the same wake (any button, instant) and the
             * same 30-minute escalation to a PMU standby. This is the rule
             * that was missing on 2026-09-24: paused, parked, dark and cold,
             * the loop still ran — twelve hours of it in one log, 3972 to
             * 3556 mV.
             *
             * `busy` is every reason to stay up: the DAC running
             * (player_playing, NOT player_active — active is true across a
             * pause), external power (nothing to save, and the Battery page
             * is what the bench watches), a sleep timer armed (a suspend
             * would disarm it). Fed every pass so the stamps stay current;
             * the answer is taken only when nothing else is sleeping us.
             * The origin is now: there is no press to time an escalation
             * from, exactly as the sleep timer's FIRE above.
             */
            {
                int idle_fire = idlesleep_feed(&g_idlesleep, nowp, L->last_input,
                                               player_playing() ||
                                               power_is_external() ||
                                               sleeptimer_armed(&g_sleep));
                if (idle_fire && !want_suspend) {
                    uart_puts("core: idle: nothing playing, no input for ");
                    uart_dec((int)(IDLESLEEP_TIMEOUT_US / 1000000u));
                    uart_puts(" s, sleeping\n");
                    want_suspend   = 1;
                    suspend_origin = nowp;
                }
            }

            if (want_suspend) {
                /* ANY suspend leaves the timer Off — in one place, so a new
                 * sleep site cannot forget one of the two sides (the FIRE
                 * above has already disarmed g_sleep; this is what keeps the
                 * ROW honest, and what disarms a running timer when PLAY is
                 * held instead). */
                sleeptimer_reset(&g_sleep);
                g_settings.sleep_timer_min = 0;

                /* Forget the transient windows too. Both are start-stamp +
                 * elapsed compares against a counter that wraps every ~71.6
                 * min (ui_window_t), and a sleep is the one thing here that
                 * can last that long: a suspend entered under the Hold banner
                 * or the volume plate could otherwise wake to a stale one for
                 * up to its full span. The wake repaints the real screen. */
                g_lock_flash.armed = 0;
                g_vol_show.armed   = 0;

                /* And the presses this loop was timing, for the same reason
                 * and in the same breath. A RIGHT/LEFT hold that the SLEEP
                 * TIMER interrupts mid-aim would otherwise read as a release
                 * on the first pass back — suspend_to_ram does not return
                 * until every button is up and the latch is drained — and
                 * commit a seek aimed before the nap, to a listener who
                 * pressed a button only to light the screen.
                 *
                 * BEFORE the call, not after it: suspend_to_ram paints and
                 * presents the wake frame itself, and np_aim_target() would
                 * find a live aim and draw the pre-nap target over the
                 * transport band for that frame. Nothing between here and the
                 * call reads these. PLAY is the exception that stays where it
                 * is: suspend WAITS on play_key's button, and on the path
                 * where it triggered the sleep its hold has already fired, so
                 * its release is silent by construction. */
                keyhold_reset(&L->menu_key);
                seekhold_reset(&g_ff);
                seekhold_reset(&g_rw);

                suspend_to_ram(suspend_origin);              /* returns on wake */
                if (g_standby_refused) {
                    /* The escalation (or the 5 s hold) asked the PMU for a
                     * standby and it refused; enter_standby stopped the
                     * player and relit the screen. Do not have the idle
                     * policy ask again every 32 minutes — once is the
                     * diagnosis. The flag itself is consumed below. */
                    idlesleep_off(&g_idlesleep);
                    uart_puts("core: idle sleep: off for this session (standby refused)\n");
                }
                L->last_input   = mmio_read32(USEC_TIMER_ADDR);
                L->last_present = L->last_input;      /* suspend just presented the wake
                                                 * frame: pace the loop's own
                                                 * repaint behind it, or the two
                                                 * back-to-back full frames meet
                                                 * a busy BCM (device log) */
                L->bl_state   = BL_FULL;           /* backlight restored on resume */
                L->panel_slept = 0;                /* suspend woke, presented and
                                                 * lit the panel itself — even
                                                 * one this loop had slept at
                                                 * idle first (lcd_sleep is
                                                 * idempotent); don't wake it
                                                 * a second time below */
                L->cpu_idled  = 0;                 /* suspend's wake cpu_boost()
                                                 * re-established the ONE boost
                                                 * this loop believes it holds.
                                                 * The sleep timer is the first
                                                 * thing that can suspend from
                                                 * the dark, idled state (a
                                                 * PLAY hold cannot: the press
                                                 * relit the screen and
                                                 * re-boosted a pass earlier),
                                                 * and leaving this set would
                                                 * make the idle block below
                                                 * boost a SECOND time on this
                                                 * same pass — g_boost stuck at
                                                 * 2, so the core never drops
                                                 * to 30 MHz at idle again */
                L->dirty      = 1;                 /* repaint the current screen */
            }
        }

        /*
         * RIGHT / LEFT and MENU, also by press length and also from LIVE
         * state, for the same reason PLAY is: how long a press lasted is the
         * only thing that tells a skip from a seek, or "back" from "home".
         *
         * RIGHT/LEFT only mean transport on the player screens, and only with
         * a track loaded; ui/gesture.c latches that at the down-edge, so the
         * press that JUMPS to Now Playing from a list cannot turn into a seek
         * the moment it lands there. A skip now happens on the release rather
         * than the down-edge — up to half a second later — which is how the
         * original behaves and how PLAY already behaves here.
         *
         * MENU's tap is NOT taken from here: it already fired at the
         * down-edge in the per-screen switch below, which is what keeps every
         * back-one instant. Only the hold is decided here.
         */
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

        /* A refused PMU standby (see enter_standby) has already relit and
         * repainted the screen from outside this loop; resync the backlight
         * and idle bookkeeping so the next press is not treated as a wake. */
        if (g_standby_refused) {
            g_standby_refused = 0;
            L->last_input = mmio_read32(USEC_TIMER_ADDR);
            L->bl_state   = BL_FULL;
            L->panel_slept = 0;              /* enter_standby's fallback already
                                           * did wake -> paint -> present ->
                                           * backlight; not a second time */
            L->dirty      = 1;
        }

        /* Hold-switch edge (a cheap GPIO read, independent of the wheel block
         * which is gated off while held): flash the Hold banner (where the
         * screen has chrome to host it) and toggle the input lock. Playback
         * is untouched. */
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

        /* Input is sampled on the 100 Hz tick and latched (clickwheel_service),
         * so a tap that lands while this loop is blocked in a disk read isn't
         * lost — we just drain the latch here. */
        wheel_event_t ev;
        int have_ev = clickwheel_get_event(&ev) ? 1 : 0;
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
            if (ev.buttons) {
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
                   (ev.buttons || ev.wheel_delta)) {
            /* The On-The-Go banner is confirmation, not a modal: the first
             * real touch ends it and THIS pass falls through to the normal
             * render with the event applied. Same rule as the UNLOCKED
             * banner below, and the reason nothing is ever applied behind it. */
            g_otg_flash.armed = 0;
            L->banner_up     = 0;
            L->dirty = 1;
        } else if (have_ev && g_lock_flash.armed && (ev.buttons || ev.wheel_delta)) {
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
        if (have_ev) {
            L->last_input = mmio_read32(USEC_TIMER_ADDR);
            /* The tick the event is timed from: a row press's length is
             * measured from HERE, not from the first pass the live sampler
             * happens to see the button down on. */
            const uint32_t ev_us = L->last_input;
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
                    if (ev.buttons & WHEEL_BTN_PLAY) {
                        keyhold_swallow_tap(&L->play_key);   /* ...its release too */
                    }
                    if (ev.buttons & WHEEL_BTN_MENU)   keyhold_void(&L->menu_key);
                    if (ev.buttons & WHEEL_BTN_SELECT) keyhold_void(&L->row_key);
                    if (ev.buttons & WHEEL_BTN_RIGHT)  seekhold_void(&g_ff);
                    if (ev.buttons & WHEEL_BTN_LEFT)   seekhold_void(&g_rw);
                    ev.buttons = 0;
                    ev.wheel_delta = 0;
                }
            }
            /* Menu click on a button press (one per down-edge; not on the
             * swallowed backlight-wake press above). Wheel clicks are emitted by
             * wheel_move itself, only when the cursor actually advances a row —
             * so a sub-threshold tick that doesn't move can't click. */
            if (ev.buttons) {
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
            if (ev.buttons || ev.wheel_delta) {
                battwarn_input(mmio_read32(USEC_TIMER_ADDR));
            }
            if ((scr_cur() == SCR_CHARGING || scr_cur() == SCR_BATTERY) &&
                ev.buttons) {
                scr_pop();
                L->dirty = 1;
                if (ev.buttons & WHEEL_BTN_PLAY) {
                    keyhold_swallow_tap(&L->play_key);   /* PLAY's release too */
                }
                /* ...and the press that got the modal off the screen must not
                 * also walk the user home or seek, so those are voided whole
                 * (same rule as the backlight wake above). */
                if (ev.buttons & WHEEL_BTN_MENU)   keyhold_void(&L->menu_key);
                if (ev.buttons & WHEEL_BTN_SELECT) keyhold_void(&L->row_key);
                if (ev.buttons & WHEEL_BTN_RIGHT)  seekhold_void(&g_ff);
                if (ev.buttons & WHEEL_BTN_LEFT)   seekhold_void(&g_rw);
                ev.buttons     = 0;
                ev.wheel_delta = 0;
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
                if (ev.buttons & WHEEL_BTN_RIGHT) {
                    seekhold_missed_tap(&g_ff, (live & WHEEL_BTN_RIGHT) != 0);
                }
                if (ev.buttons & WHEEL_BTN_LEFT) {
                    seekhold_missed_tap(&g_rw, (live & WHEEL_BTN_LEFT) != 0);
                }
            } else if (ev.buttons & WHEEL_BTN_RIGHT) {
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
                    ev.buttons = (uint8_t)(ev.buttons & ~WHEEL_BTN_RIGHT);
                    /* The switch below runs on the NEW top. A thumb landing on
                     * the ring to press RIGHT can latch a sub-detent delta in
                     * the same tick; on Now Playing that reads as a volume
                     * nudge (+ the plate). The press was a jump, nothing else. */
                    ev.wheel_delta = 0;
                }
            }
            switch (scr_cur()) {
            case SCR_MENU:
                main_menu_input(&ev, L, fs, ev_us);
                break;

            case SCR_MUSIC:
                music_menu_input(&ev, L, fs, ev_us);
                break;

            case SCR_ARTISTS:
                artists_input(&ev, L, fs, ev_us);
                break;

            case SCR_SONGS:
                songs_input(&ev, L, fs, ev_us);
                break;

            case SCR_GENRES:
                genres_input(&ev, L, fs, ev_us);
                break;

            case SCR_PLAYLISTS:
                if (ev.wheel_delta) {
                    g_pl_sel = wheel_move(g_pl_sel, playlists_row_count(),
                                          ev.wheel_delta, &g_pl_accum);
                    L->dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    /* Row 0 is the pinned On-The-Go list; the rest are the
                     * files. No hold action here, so SELECT still acts on the
                     * down-edge — a delayed tap would be latency for nothing. */
                    if (g_pl_sel == 0) {
                        g_otg_free_slot = otg_slot_free_index(fs);
                        g_otg_sel = (g_otg.n > 0) ? OTG_ROW_FIRST : OTG_ROW_CLEAR;
                        g_otg_accum = 0;
                        g_otg_confirm.armed = 0;
                        scr_push(SCR_OTG);
                    } else {
                        playlist_open(fs, g_pl_sel - 1); /* parse + resolve + bind */
                        scr_push(SCR_PLAYLIST);
                    }
                    L->dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back (Music or main) */
                    L->dirty = 1;
                }
                break;

            case SCR_PLAYLIST:
                if (ev.wheel_delta && playlist_row_count() > 0) {
                    g_plt_sel = wheel_move(g_plt_sel, playlist_row_count(),
                                           ev.wheel_delta, &g_plt_accum);
                    g_pl_confirm.armed = 0;     /* moving off the row cancels it */
                    L->dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && playlist_row_count() > 0) {
                    /* Tap plays (or deletes a saved slot); hold adds the track
                     * to On-The-Go. */
                    rowsel_arm(g_plt_sel, &L->row_key, ev_us);
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Playlists */
                    L->dirty = 1;
                }
                break;

            case SCR_OTG:
                /* Both action rows are greyed and do nothing on an empty
                 * list, so there is nothing to move to and nothing to press:
                 * the cursor stays on row 0 and the wheel is silent. */
                if (ev.wheel_delta && g_otg.n > 0) {
                    g_otg_sel = wheel_move(g_otg_sel, otg_row_count(),
                                           ev.wheel_delta, &g_otg_accum);
                    g_otg_confirm.armed = 0;    /* moving off the row cancels it */
                    L->dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_otg.n > 0) {
                    /* Tap plays from the row (or clears / saves); hold removes
                     * the row. */
                    rowsel_arm(g_otg_sel, &L->row_key, ev_us);
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Playlists */
                    L->dirty = 1;
                }
                break;

            case SCR_SEARCH: {
                if (ev.wheel_delta) {
                    if (g_search.mode == SEARCH_PICK) {
                        /* One cell a detent, wrapping; the click is ours
                         * because the ring is not a list and does not go
                         * through wheel_move. */
                        if (search_ring_move(&g_search, ev.wheel_delta)) {
                            L->dirty = 1;
                        }
                    } else if (g_search.nhit > 0) {
                        g_search.sel = wheel_move(g_search.sel, g_search.nhit,
                                                  ev.wheel_delta, &g_search.accum);
                        L->dirty = 1;
                    }
                }
                int act = SEARCH_ACT_NONE;
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    /* A hit is a row: the tap opens it, and a hold on a SONG
                     * hit adds it to On-The-Go like any other song row. The
                     * ring is not a list — its SELECT types a character — so
                     * search_hold_rows() reports no rows there and the press
                     * keeps the down-edge, as a key must. */
                    if (search_hold_rows(&g_search, 0, 0) > 0) {
                        rowsel_arm(g_search.sel, &L->row_key, ev_us);
                    } else {
                        act = search_key(&g_search, SEARCH_KEY_SELECT);
                    }
                } else if (ev.buttons & WHEEL_BTN_MENU) {
                    act = search_key(&g_search, SEARCH_KEY_MENU);
                } else if (ev.buttons & WHEEL_BTN_RIGHT) {
                    act = search_key(&g_search, SEARCH_KEY_RIGHT);
                } else if (ev.buttons & WHEEL_BTN_LEFT) {
                    act = search_key(&g_search, SEARCH_KEY_LEFT);
                }
                switch (act) {
                case SEARCH_ACT_RESCAN:
                    search_rescan();
                    L->dirty = 1;
                    break;
                case SEARCH_ACT_TO_RESULTS:
                case SEARCH_ACT_TO_PICK:
                    L->dirty = 1;
                    break;
                case SEARCH_ACT_POP:
                    scr_pop();                          /* back to Music menu */
                    L->dirty = 1;
                    break;
                case SEARCH_ACT_OPEN:
                    if (search_open_hit(fs)) L->np_first = 1;
                    L->dirty = 1;
                    break;
                default:
                    break;
                }
                break;
            }

            case SCR_BROWSER: {
                /* Depth 0 scrolls the album list (g_br_sel); depth 1 the loaded
                 * tracklist (g_det_sel) — separate so backing out restores the
                 * album-list position. */
                int count = (g_dir_depth == 0) ? albumlist_count() : g_browse_n;
                int *sel  = (g_dir_depth == 0) ? &g_br_sel   : &g_det_sel;
                int *acc  = (g_dir_depth == 0) ? &g_br_accum : &g_det_accum;
                if (ev.wheel_delta && count > 0) {
                    *sel = wheel_move(*sel, count, ev.wheel_delta, acc);
                    L->dirty = 1;                    /* window derived at paint time  */
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && count > 0) {
                    /* Tap enters the album (or plays the track); hold adds the
                     * whole album, or that one track, to On-The-Go. */
                    rowsel_arm(*sel, &L->row_key, ev_us);
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_dir_depth > 0 && g_br_from_search) {
                        /* Opened straight from a Search hit: back is the
                         * results, not an album list that was never shown.
                         * The depth goes with the screen, as it does in
                         * scr_pop_to_root — this is the one exit from depth 1
                         * that leaves no BROWSER on the stack to own it. */
                        g_br_from_search = 0;
                        g_dir_depth      = 0;
                        scr_pop();
                    } else if (g_dir_depth > 0) {       /* tracklist -> album list */
                        g_dir_depth = 0;
                        albumlist_queue_chips();        /* g_br_sel kept (the album) */
                    } else {                            /* album list -> Music menu */
                        scr_pop();
                    }
                    L->dirty = 1;
                }
                break;
            }

            case SCR_NOWPLAYING:
                /* Scrubbing: the wheel aims at a position instead of setting
                 * volume. Accelerates with the spin so a 70-minute track is
                 * crossable, but one deliberate detent is always SCRUB_STEP_S. */
                if (ev.wheel_delta && np_scrubbing() && player_active()) {
                    int vel   = wheel_accel_step(ev.wheel_delta);
                    int units = ev.wheel_delta / CW_WHEEL_SENSITIVITY;
                    if (units == 0) {
                        units = (ev.wheel_delta > 0) ? 1 : -1;
                    }
                    int32_t  step  = (int32_t)units * SCRUB_STEP_S * vel;
                    int32_t  t     = (int32_t)g_scrub_target_s + step;
                    uint32_t total = player_total_s();
                    if (t < 0) t = 0;
                    if (total > 0 && t > (int32_t)total) t = (int32_t)total;
                    g_scrub_target_s = (uint32_t)t;
                    g_scrub_last_us  = mmio_read32(USEC_TIMER_ADDR);
                    g_scrub_dirty    = 1;
                    /* Only the transport band shows the target: force ITS
                     * repaint (the clock-tick path) — not `dirty`, which
                     * would re-render and push the whole frame. */
                    L->np_last          = 0xFFFFFFFFu;
                } else if (ev.wheel_delta) {             /* wheel = volume        */
                    /* The wheel reports raw position units (~CW_WHEEL_SENSITIVITY
                     * per detent), which is why volume used to leap ~10 at a time.
                     * Quantise to whole detents: a single slow detent nudges the
                     * volume by exactly ±1, and spinning faster (more detents per
                     * event) accelerates so you can still sweep the whole range. */
                    /* The rule now lives in ui/wheel.c (wheel_value_step) and
                     * every value wheel shares it: 1 detent = 1, faster
                     * sweeps (2 -> 4, 3 -> 6), capped at 12. */
                    int step = wheel_value_step(ev.wheel_delta,
                                                WHEEL_VALUE_STEP_MAX);
                    /* The Volume Limit is applied by the same helper the
                     * Settings slider uses — one rule, two wheels. No click
                     * either way: this is a slider, not navigation. */
                    int nv = settings_volume_clamp(&g_settings, g_volume + step);
                    if (nv != g_volume) {
                        g_volume = nv;
                        hal_volume_set(g_volume);
                        g_settings.volume = g_volume;     /* keep Settings in sync */
                        settings_touch(); /* debounced: one write per volume sweep */
                    }
                    /* Armed even when the wheel could not move: at a rail —
                     * and especially AT THE LIMIT, where the plate's triangle
                     * is the explanation — the user still needs to see why. A
                     * pinned wheel no longer earns a disk write, though. */
                    ui_window_arm(&g_vol_show);
                    /* The plate is its own present (see the Now Playing
                     * branch of the render step): not `dirty`, which would
                     * push all 38400 words for a 3200-word rect. */
                    L->np_vol_dirty = 1;
                }
                /* SELECT is now press-length sensitive here: a tap toggles the
                 * scrubber (what SELECT does on Now Playing on a real iPod, and
                 * far and away the more frequent action), a hold opens the queue
                 * view. Only the down-edge is recorded — the decision is made in
                 * the per-pass block below, once the press length is known. */
                if ((ev.buttons & WHEEL_BTN_SELECT) && player_active()) {
                    g_sel_down_us = mmio_read32(USEC_TIMER_ADDR);
                    g_sel_pending = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back, keep playing */
                    L->dirty = 1;
                }
                break;

            case SCR_QUEUE:
                queue_input(&ev, L, fs, ev_us);
                break;

            case SCR_SETTINGS: {
                /*
                 * The Date & Time EDITOR is not a list: the wheel moves a
                 * field's value, SELECT confirms a field and the last one
                 * commits, MENU writes nothing at all. ui/settime.c owns every
                 * one of those decisions (and every calendar edge case in
                 * them); this arm is the wiring.
                 */
                if (g_set_screen == SETTINGS_SETTIME) {
                    if (ev.wheel_delta) {
                        /* Detents, not raw ticks: one slow detent is one
                         * minute / hour / day (it was ~4). */
                        settime_adjust(&g_settime,
                                       wheel_value_step(ev.wheel_delta,
                                                        WHEEL_VALUE_STEP_MAX));
                        L->dirty = 1;
                    }
                    if (ev.buttons & WHEEL_BTN_SELECT) {
                        if (settime_next(&g_settime)) {
                            /* The last field: write the clock. The editor
                             * shows LOCAL time and the chip holds UTC, so the
                             * device's stored offset comes back off here. */
                            datetime_t civil;
                            settime_civil(&g_settime, &civil);
                            /* The editor shows WALL time; the device keeps
                             * standard (Daylight Saving, kernel/datetime.h). */
                            uint32_t local = clock_dst_standard_from_wall(
                                                 datetime_to_epoch(&civil),
                                                 g_settings.dst_on);
                            uint32_t utc   = 0;
                            /* The editor clamps the YEAR to 2001..2099; the
                             * zone can still push the UTC epoch off either end
                             * (2001-01-01 00:30 at UTC+02:00 is 2000 in UTC).
                             * datetime_utc_from_local refuses that rather than
                             * wrapping, and the refusal takes the same path as
                             * a failed write: the old time stands and the log
                             * says so. */
                            int rc = datetime_utc_from_local(
                                         local, g_settings.utc_off_min, &utc)
                                     ? hal_rtc_set(utc) : -3;
                            uart_puts("core: rtc set ");
                            uart_put_hex32(utc);
                            uart_puts(" rc ");
                            uart_put_hex32((uint32_t)rc);
                            uart_putc('\n');
                            if (rc == 0) {
                                wallclock_anchor(&g_wclock, 1, utc,
                                                 mmio_read32(USEC_TIMER_ADDR));
                                g_clock_tick_us   = mmio_read32(USEC_TIMER_ADDR);
                                g_clock_resync_us = g_clock_tick_us;
                                g_clock_min       = 0;   /* repaint the strip */
                            }
                            /* A manual set NEVER touches applied_epoch: a
                             * later host stamp is still news (timesync.h). */
                            g_set_screen = SETTINGS_DATETIME;
                            g_set_sel = g_set_accum = 0;
                        }
                        L->dirty = 1;
                    }
                    if (ev.buttons & WHEEL_BTN_MENU) {
                        g_set_screen = SETTINGS_DATETIME;   /* nothing written */
                        g_set_sel = g_set_accum = 0;
                        L->dirty = 1;
                    }
                    break;
                }

                int scount = settings_count(g_set_screen);
                int slider = (settings_kind(g_set_screen, g_set_sel)
                              == SETTINGS_KIND_SLIDER);
                /* NB `slider` is re-read before the SELECT test below: one latched
                 * event can carry BOTH a wheel move and a button, and the move
                 * changes which row SELECT lands on. */
                if (ev.wheel_delta) {
                    if (g_set_editing && slider) {         /* adjust the value  */
                        /* Detents, not raw ticks: this was the raw delta
                         * clamped to +/-4, so ONE slow detent (~4 ticks)
                         * moved every slider by four and no single value
                         * was reachable. The volume wheel's rule now. */
                        int dd = wheel_value_step(ev.wheel_delta,
                                                  WHEEL_VALUE_STEP_MAX);
                        /* Only a value that MOVED earns an apply and a disk
                         * write: a wheel pinned at a rail (volume 100,
                         * brightness 32) is not a change — settings_adjust
                         * says so, and a48510f made SELECT honour the same
                         * answer. */
                        if (settings_adjust(g_set_screen, &g_settings, g_set_sel, dd)) {
                            settings_apply();              /* live volume/etc.  */
                            settings_touch();              /* debounced save    */
                            /* Brightness slider: light up to the new level as
                             * you turn, so the wheel drives the panel in real
                             * time. */
                            if (g_set_screen == SETTINGS_DISPLAY && g_set_sel == 1) {
                                if (!L->panel_slept) {   /* the Panel-wake block
                                                       * lights it after the
                                                       * first present, else
                                                       * a white flash */
                                    backlight_set(g_settings.backlight_bright);
                                }
                                L->bl_state = BL_FULL;
                            }
                        }
                    } else if (scount > 0) {               /* move selection    */
                        g_set_sel = wheel_move(g_set_sel, scount,
                                               ev.wheel_delta, &g_set_accum);
                    }
                    L->dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    slider = (settings_kind(g_set_screen, g_set_sel)
                              == SETTINGS_KIND_SLIDER);    /* the CURRENT row   */
                    if (slider) {
                        /* A locked slider (Bass/Treble under an EQ preset) is
                         * a readout: the click happens, edit mode does not. */
                        if (!settings_row_locked(g_set_screen, &g_settings,
                                                 g_set_sel)) {
                            g_set_editing = !g_set_editing; /* enter/exit edit  */
                        }
                    } else {
                        int act = settings_activate(g_set_screen, &g_settings,
                                                    g_set_sel);
                        int target = -1;
                        switch (act) {
                        case SETTINGS_ENTER_PLAYBACK: target = SETTINGS_PLAYBACK; break;
                        case SETTINGS_ENTER_SOUND:    target = SETTINGS_SOUND;    break;
                        case SETTINGS_ENTER_DISPLAY:  target = SETTINGS_DISPLAY;  break;
                        case SETTINGS_ENTER_ABOUT:    target = SETTINGS_ABOUT;    break;
                        case SETTINGS_ENTER_THEME:    target = SETTINGS_THEME;    break;
                        case SETTINGS_ENTER_CLICKER:  target = SETTINGS_CLICKER;  break;
                        case SETTINGS_ENTER_DIAG:     target = SETTINGS_DIAG;     break;
                        case SETTINGS_ENTER_BATTERY:  target = SETTINGS_BATTERY;  break;
                        case SETTINGS_ENTER_DATETIME: target = SETTINGS_DATETIME; break;
                        case SETTINGS_ENTER_SETTIME:
                            /* Seed the editor from the best time we have —
                             * the running clock, else the host's stamp, else
                             * ui/settime.c's own fixed default — and enter it
                             * WITHOUT touching g_set_root_sel: the root's
                             * remembered row belongs to Date & Time, which is
                             * where MENU comes back to. */
                            {
                                datetime_t seed;
                                int have = clock_local_now(&seed);
                                if (!have && g_settings.host_epoch != 0) {
                                    have = datetime_local(g_settings.host_epoch,
                                                          g_settings.host_off_min,
                                                          &seed);
                                }
                                settime_begin(&g_settime, have, &seed,
                                              !g_settings.time_24h);
                            }
                            g_set_screen = SETTINGS_SETTIME;
                            g_set_sel = g_set_accum = 0;
                            g_set_editing = 0;
                            break;
                        default: break;
                        }
                        if (target >= 0) {                 /* descend a screen  */
                            g_set_root_sel = g_set_sel;
                            g_set_screen = target;
                            /* A picker opens on the value in force. */
                            g_set_sel = settings_initial_sel(target, &g_settings);
                            g_set_accum = 0;
                            g_set_editing = 0;
                        } else if (act == SETTINGS_ACTION_DISKMODE) {
                            /* Reboot into the ROM's USB mass-storage mode. We
                             * implement no USB ourselves, so this is the only
                             * way onto a host from inside our own firmware —
                             * the alternative is catching the ROM's Select+Play
                             * window at boot, which is timing-sensitive and easy
                             * to miss. Stop playback and blank the panel first
                             * so we never reset mid-DMA or leave a half-drawn
                             * screen; power_enter_disk_mode never returns. */
                            player_stop();
                            hal_audio_close();
                            /* Disk mode is where the log gets READ, so
                             * what is still in RAM has to land first —
                             * the reboot below is a ROM entry, nothing
                             * of ours runs again. Same for a settings
                             * change made a second ago. Both forced;
                             * the drive is up (the ROM needs it anyway).
                             * DEVICE 2026-09-13: a whole test session
                             * arrived at the host as "prev unflushed". */
                            settings_commit(1);
                            otg_commit(CFG_COMMIT_FORCE);
                            uart_puts("core: disk mode: entering\n");
                            evlog_commit(CFG_COMMIT_FORCE);
                            console_clear(LINEN_SURFACE);
                            ui_text_centered(LCD_HEIGHT / 2 - 8, "Disk Mode",
                                             FONT_TITLE, LINEN_INK);
                            lcd_present_fb(console_framebuffer());
                            power_enter_disk_mode();
                        } else if (act == SETTINGS_ACTION_RESET) {
                            /* THE CLOCK IS NOT A SETTING. settings_defaults()
                             * zeroes all six of its fields (it is also the
                             * pre-load state, where a non-zero stamp would be
                             * one nobody wrote), so the reset puts back the
                             * three that are facts rather than preferences:
                             * the host's stamp and offset, which the device may
                             * not have acted on yet, and the DISPLAY OFFSET,
                             * without which the clock on the strip, in the
                             * header and in the editor would read UTC for the
                             * rest of the session — and a manual set made in
                             * that window would write local-as-UTC to the chip
                             * and jump by the zone at the next boot.
                             *
                             * The MARK is deliberately not restored: the next
                             * boot then re-examines the stamp (rule 5 decides
                             * whether it is still worth applying), which is
                             * what makes "a Reset does not cost you the clock"
                             * true rather than a hope. What DOES reset are the
                             * two rows: 12-hour, no clock in the title. */
                            uint32_t host_epoch = g_settings.host_epoch;
                            int      host_off   = g_settings.host_off_min;
                            int      disp_off   = g_settings.utc_off_min;
                            int      host_dst   = g_settings.host_dst_raw;
                            settings_defaults(&g_settings);
                            g_settings.host_epoch   = host_epoch;
                            g_settings.host_off_min = host_off;
                            g_settings.utc_off_min  = disp_off;
                            g_settings.host_dst_raw = host_dst;
                            settings_apply();
                            sleep_timer_apply();           /* defaults() zeroed
                                                            * the row: disarm  */
                            settings_touch();              /* persist the reset */
                        } else if (act == SETTINGS_ACTION_SLEEPTIMER) {
                            /* The record changed, but only its RUNTIME part:
                             * re-arm the countdown and DO NOT touch. Nothing
                             * on disk stores the duration (ui/settings.h), so
                             * a touch here would spin the drive up three
                             * seconds later to write a byte-identical record —
                             * and it would do it at bedtime, which is the one
                             * moment the drive is parked for the night. */
                            sleep_timer_apply();
                        } else if (act == SETTINGS_ACTION_NONE) {
                            /* Only a row that actually CHANGED the record gets
                             * persisted. SELECT on About/Diagnostics, or on the
                             * theme already selected, used to mark the config
                             * dirty and spin the drive up three seconds later to
                             * write a byte-identical record. */
                            settings_apply();
                            /* Shuffle changed under a playing queue means the
                             * player just dealt a NEW order; the record still
                             * carries the old (seed, keep) until the next
                             * capture edge (a track change, a pause, five
                             * minutes), and a boot inside that window would
                             * deal a valid but different order. Capture now.
                             * resume_capture() is idempotent — it touches only
                             * when something actually moved — so the rows that
                             * are not about playback pay nothing. */
                            resume_capture();
                            settings_touch();
                        }
                    }
                    L->dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_set_editing) {
                        g_set_editing = 0;                 /* exit edit, stay   */
                    } else if (g_set_screen != SETTINGS_ROOT) {
                        g_set_screen = SETTINGS_ROOT;      /* back to root      */
                        g_set_sel = g_set_root_sel;
                        g_set_accum = 0;
                    } else {
                        scr_pop();                          /* leave Settings    */
                        /* The natural "I'm done" moment: commit now rather than
                         * waiting out the debounce, so the record is on the
                         * platter before the user can reach for the Hold switch.
                         * Same exit the MENU hold takes (scr_pop_to_root). */
                        settings_leave();
                    }
                    L->dirty = 1;
                }
                break;
            }

            case SCR_BATTERY:
            case SCR_CHARGING:
                break;                        /* dismissal is handled above */
            }
        }

        /* Settings > Theme's live preview, or the saved theme back once the
         * picker is left without SELECT. A swap repaints the whole panel. */
        if (theme_preview_sync()) L->dirty = 1;

        /* Idle-timeout backlight: dim, then off — timeout from Settings (0 =
         * never), brightness from Settings. Playback keeps running. The
         * timing is kernel/backlightpolicy.h; the LED and the panel are here. */
        uint32_t idle = mmio_read32(USEC_TIMER_ADDR) - L->last_input;
        int bl_step = backlightpolicy_step(L->bl_state, idle,
                                           g_settings.backlight_secs);
        if (bl_step == BLP_DIM) {
            backlight_set(backlightpolicy_dim_level(g_settings.backlight_bright));
            L->bl_state = BL_DIM;
        } else if (bl_step == BLP_OFF) {
            backlight_set(0);
            L->bl_state = BL_OFF;
            if (PANEL_SLEEP_AT_IDLE) {    /* see the note above run_ui() */
                /* LED first (above), then the panel: the sleep drains any
                 * in-flight update, drops the panel-enable bits and issues
                 * LCD_SLEEP, ~20 ms of which is a settle the loop sits out
                 * (the pump is not called, but the PCM ring is far deeper
                 * than that). From here every present is refused until the
                 * "Panel wake" block — and nothing here tries: every painter
                 * is gated on bl_state != BL_OFF. */
                lcd_sleep();              /* blank the panel too, not just the LED */
                L->panel_slept = 1;
                L->panel_refused_at_sleep = lcd_presents_refused();
            }
        }

        /* Idle CPU-clock scaling: once the screen has timed fully off AND nothing
         * is playing, drop the core 80->30 MHz — only the disk/decode path needs
         * 80. Restore the instant there's life again: any wake back to BL_FULL, or
         * playback starting. Rides the same refcounted boost the boot path took, so
         * it stays balanced (idle unboost 1->0, wake boost 0->1) and re-boosts
         * before the render/decode work later in this same iteration.
         *
         * "Playing" means the DAC is running — player_playing(), NOT
         * player_active(). Active stays set across a pause (the UI needs it to,
         * every transport control is gated on it), so keying this on active
         * meant a PAUSED device with the screen off held 80 MHz indefinitely:
         * the single largest idle-power miss in the firmware, since pause-and-
         * pocket is the common way to stop listening. A paused pump does no
         * decode and feeds no DMA, so 30 MHz is plenty; the resume itself
         * always arrives through a wake press that has already re-lit the
         * screen (and so re-boosted here) one pass earlier. The rule is
         * kernel/backlightpolicy.h. */
        int cpu_step = backlightpolicy_cpu(L->cpu_idled, L->bl_state,
                                           player_playing());
        if (cpu_step == BLP_CPU_BOOST) {
            cpu_boost();
            L->cpu_idled = 0;
        } else if (cpu_step == BLP_CPU_IDLE) {
            cpu_unboost();
            L->cpu_idled = 1;
        }

        /* Disk spin-down at idle: once nothing needs the drive AND the user has
         * been idle a while, park the platters — the single largest continuous
         * draw on a spinning-HDD unit. "Nothing needs it" = not actively playing;
         * PAUSED counts as idle: the player's paused pump stocks the anti-skip
         * buffer to its low mark while the platters are still up (this park
         * comes 20 s later), so a resume plays from RAM and the drive spins
         * up lazily on the next burst. When the buffer is shallow anyway
         * (paused and parked a second into a track), player_resume pre-pays
         * the spin-up before the DAC starts. DEVICE 2026-09-13: before that,
         * a track restored paused at boot resumed into a synchronous read
         * on this parked drive — 11 underruns. Skipped while actively
         * playing: player.c already parks the drive between bursts and owns that
         * cadence. ata_is_parked() is the shared truth, so we never re-issue
         * STANDBY on an already-parked drive. On an iFlash/SSD mod this is a
         * harmless no-op. */
        /* Debounced settings write. Deliberately placed BEFORE the spin-down
         * check below: if a save is due we want it to land while the platters
         * are still turning, rather than parking the drive and immediately
         * spinning it back up for a 1 KB write. Nothing happens here unless a
         * setting actually changed and the user has since gone quiet. */
        settings_commit(0);
        /* The On-The-Go list rides the same idle pass and the same gate: a
         * change made with the drive parked waits for the platters to turn
         * for some other reason, or for one of the forced commits above. */
        otg_commit(CFG_COMMIT_IDLE);
        /* The event log's idle flush, same placement for the same reason:
         * a full block lands while the platters still turn. Never wakes a
         * parked drive; at most one block per pass. */
        evlog_commit(CFG_COMMIT_IDLE);

        /* The idle spin-down, the lock park, the refused-STANDBY retry and
         * the browsing hold handed to the player: kernel/drivepower.h. */
        uint32_t now_d = mmio_read32(USEC_TIMER_ADDR);
        drivepower_in_t dp_in;
        dp_in.now_us      = now_d;
        dp_in.idle_us     = idle;
        dp_in.locked      = g_locked;
        dp_in.playing     = player_playing();
        dp_in.parked      = ata_is_parked();
        dp_in.player_busy = player_disk_busy();
        drivepower_out_t dp = drivepower_idle_feed(&g_drivepower, &dp_in);
        if (dp.standby) {
            if (dp.flush_first) {
                /* The idle path waits out a 3 s debounce and never wakes a
                 * parked drive, so a setting changed just before the lock
                 * would sit pending until the next spin-up. Write it while
                 * the platters still turn. */
                settings_commit(CFG_COMMIT_SOFT);
                otg_commit(CFG_COMMIT_SOFT);
            }
            drivepower_standby_result(&g_drivepower, ata_standby(), now_d);
        }
        player_hold_drive(dp.hold);
        disk_park_track(player_playing());

        /*
         * SELECT press-length arbitration on a LIST ROW: the tap acts on
         * release, the hold at SEL_HOLD_US adds to (or removes from)
         * On-The-Go. See the rowsel_* block for why the row is recorded at the
         * down-edge rather than read live.
         *
         * The machine is fed EVERY pass, pending or not, so a release is
         * always seen: stop feeding it after an action and the next press
         * would be timed from the old press's origin and fire HOLD instantly.
         * Only the ACTING is conditional.
         */
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

        /*
         * SELECT press-length arbitration (Now Playing). Decided here rather
         * than at the down-edge because the length is only known once the button
         * is released or the hold threshold passes. Held past SEL_HOLD_US opens
         * the queue immediately (so the gesture confirms itself under your
         * thumb); released before it toggles the scrubber.
         */
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
            uint32_t sel_held = mmio_read32(USEC_TIMER_ADDR) - g_sel_down_us;
            int      down     = (clickwheel_buttons() & WHEEL_BTN_SELECT) != 0;
            if (down && sel_held >= SEL_HOLD_US) {
                g_sel_pending = 0;
                scrub_exit();
                g_queue_sel   = player_queue_current();
                g_queue_accum = 0;
                scr_push(SCR_QUEUE);
                L->dirty = 1;
            } else if (!down) {
                g_sel_pending = 0;
                if (np_scrubbing()) scrub_exit();
                else                scrub_enter();
                /* Nothing above the transport band knows about the scrubber
                 * (nowplaying_render only consults it there), so a forced
                 * transport repaint is the whole change — no `dirty`. */
                L->np_last = 0xFFFFFFFFu;
            }
        }

        /*
         * Deferred seek. The target is committed once the wheel has been quiet
         * for SCRUB_COMMIT_US, so dragging across a track costs ONE seek rather
         * than one per detent — a seek stops the DAC, re-primes the ring, and on
         * a backward jump rewinds the file. Scrub mode then lapses on its own,
         * so the wheel cannot be left silently controlling position.
         */
        if (np_scrubbing()) {
            uint32_t quiet = mmio_read32(USEC_TIMER_ADDR) - g_scrub_last_us;
            if (g_scrub_dirty && quiet >= SCRUB_COMMIT_US) {
                g_scrub_dirty = 0;
                if (player_seek_to(g_scrub_target_s) != 0) {
                    g_scrub_target_s = player_elapsed_s();   /* refused: snap back */
                }
                L->np_last = 0xFFFFFFFFu;          /* transport band only, as above */
            } else if (!g_scrub_dirty && quiet >= SCRUB_EXIT_US) {
                scrub_exit();
                L->np_last = 0xFFFFFFFFu;
            }
            if (!player_active()) {
                scrub_exit();                    /* track ended under the scrubber */
            }
        }

        /* Panel wake: the instant we leave the fully-off state, bring the panel
         * back BEFORE any other present can happen this pass (the Hold-banner
         * flash or the render block below), in the only order that is safe and
         * the same one suspend_to_ram / enter_standby use:
         *
         *   lcd_wake()            panel-enable bits back, absorb armed;
         *   paint_current_screen  the real screen, rendered while still dark;
         *   lcd_present_fb        a FULL frame — its LCD_UPDATE carries the
         *                         BCM's ~500 ms panel init and the call does
         *                         not return until that has retired;
         *   backlight_set         only now, so the init is never seen (light
         *                         over it is the white flash, 02-lcd.md).
         *
         * FULL on purpose: every present while slept was refused, so the BCM's
         * framebuffer holds the last frame from BEFORE the sleep, and a partial
         * (two list rows, the transport band) on top of that would leave stale
         * pixels everywhere else. One central block covers every wake path, so
         * no input site lights the LED itself when panel_slept is set.
         *
         * dirty stays set: the render block below repaints once more through
         * the ordinary path, which is what re-registers the marquee, the
         * partial-paint caches (g_lp, np_last) and the toast/A-Z edges. That
         * second frame is invisible (same pixels) and costs one present; it is
         * exactly what suspend's return does too. */
        if (L->panel_slept && L->bl_state != BL_OFF) {
            uint32_t refused = lcd_presents_refused() - L->panel_refused_at_sleep;
            lcd_wake();
            L->panel_slept = 0;
            paint_current_screen();
            lcd_present_fb(console_framebuffer());
            backlight_set(g_settings.backlight_bright);
            L->dirty = 1;                    /* never resume onto a stale panel */
            if (refused) {
                /* Something presented into the slept panel. Not a fault (the
                 * frame above made up for it), but on the idle path nothing
                 * should — say so, so the device tells us who. */
                uart_puts("core: panel wake: ");
                uart_dec((int)refused);
                uart_puts(" present(s) refused while slept\n");
            }
        }

        /* A two-press confirm that lapsed unpressed has to un-say itself: the
         * row's label reverts to "Clear Playlist", so the screen it is on has
         * to repaint. ui_window_up DISARMS an expired window, so polling it
         * here is the edge — without this the row kept saying "Select again"
         * until something else happened to redraw it. */
        if (g_otg_confirm.armed && !otg_confirm_up(&g_otg_confirm)) L->dirty = 1;
        if (g_pl_confirm.armed  && !otg_confirm_up(&g_pl_confirm))  L->dirty = 1;

        /* The Hold banner holds the top chrome for ~1s on a Hold edge (and on
         * a button press while locked). Paint the context + banner once, hold
         * it, then repaint underneath when it fades. Suppresses the normal
         * render while up — which is why the input block above disarms the
         * window on the first event after an UNLOCK: the banner would otherwise
         * eat the render for the rest of its second while happily applying
         * every scroll and press behind it. The paint is always full; the PRESENT is the band
         * alone unless something else is pending — a Hold edge marks the strip
         * dirty for the padlock, and a wake from dark has a stale panel — in
         * which case the whole frame goes, as it always did. */
        uint32_t now_us = mmio_read32(USEC_TIMER_ADDR);
        /* Two banners can want the band at once (a Hold edge during an
         * On-The-Go confirmation). The lock one wins — it reports a state
         * change the user must see — and the other expires underneath;
         * nothing is applied unseen either way, because the input drain
         * disarms the On-The-Go banner on the first press. Both windows are
         * polled exactly once: ui_window_up DISARMS an expired one.
         *
         * A dark modal hosts neither: a window armed on the screen under it
         * (a Hold flip, then the cable, inside one second) is dropped here
         * rather than painted as a linen strip over the field — and so
         * cannot be replayed on the screen the modal pops back to. */
        if (!banner_hosted()) {
            g_lock_flash.armed = 0;
            g_otg_flash.armed  = 0;
        }
        int lock_up = ui_window_up(&g_lock_flash, LOCK_FLASH_US, now_us);
        int otg_up  = ui_window_up(&g_otg_flash,  OTG_FLASH_US,  now_us);
        if (lock_up || otg_up) {
            if (!L->banner_up && L->bl_state != BL_OFF) {
                paint_current_screen();
                if (lock_up) lock_banner_render(g_locked);
                else         otg_banner_render();
                if (L->dirty) {
                    lcd_present_fb(console_framebuffer());
                    L->dirty = 0;
                } else {
                    lcd_present_rect(console_framebuffer(), 0, 0, LCD_WIDTH,
                                     top_banner_h());
                }
            }
            L->banner_up = 1;
            /* The banner skips the render below — and with it the loop's two
             * halts, so it halts here itself. Idle (which includes paused: no
             * DMA to pace) parks the core for a tick, as the main 10 ms halt
             * does. PLAYING takes the same 200 us halt as the bottom of the
             * loop, under the same "the pump did nothing" gate: without it a
             * Hold flip mid-track free-spun the core at 80 MHz for the whole
             * second the banner is up, re-polling the wheel through masked IRQs
             * — the exact spin the bottom halt exists to prevent. */
            if (!player_playing()) {
                cpu_wait_ms(10);
            } else if (pump_us < 200u) {
                cpu_wait_us(200);
            }
            continue;                     /* skip the normal render this pass      */
        }
        if (L->banner_up) {              /* banner just faded: repaint underneath */
            L->banner_up = 0;
            L->dirty = 1;
        }

        /* A-Z locator: while the wheel is being spun fast, show the selected
         * row's initial so you can aim at a letter instead of reading rows that
         * are flying past. Its appearance/disappearance is itself a repaint. */
        char az_letter = wheel_accelerating() ? list_sel_initial() : 0;
        /* Suppressed with the backlight: a toast nobody can see is only a
         * repaint. It reappears on the next sample if the cell is still low. */
        int  toast     = (L->bl_state != BL_OFF) &&
                         battwarn_toast_up(mmio_read32(USEC_TIMER_ADDR));

        /* Render the current screen (skipped entirely when the screen is off —
         * saves the IRQ-masked present while music plays dark). Now Playing
         * repaints ~1/s for the clock; menus/browser only on change. */
        if (L->bl_state == BL_OFF) {
            /* nothing to draw */
        } else if (scr_cur() == SCR_NOWPLAYING) {
            uint32_t nowv    = mmio_read32(USEC_TIMER_ADDR);
            uint32_t elapsed = player_elapsed_s();
            int vol_active = ui_window_up(&g_vol_show, VOL_SHOW_US, nowv);
            /* The volume plate needs a push when it appears, when the wheel
             * moves it while it is up, and when it fades (to put back what it
             * covered). None of those is a full-frame event. */
            int vol_edge   = (vol_active != L->np_vol_prev) ||
                             (vol_active && L->np_vol_dirty);

            /* A FULL repaint is needed only on a real change (dirty — which the
             * loop's track-change edge sets for us) or a toast edge. Everything
             * else is a partial: the clock tick redraws ONLY the transport strip
             * (elapsed/remaining/progress) and presents only its band, and a
             * volume edge presents only the plate rect, instead of re-rendering
             * the whole screen — art, metadata and two anti-aliased bars — and
             * pushing all 38400 words for a 3200-word plate. */
            int want_full = L->dirty || toast != L->toast_prev;
            if (want_full || vol_edge) {
                if (L->np_first || !player_active() ||
                    (uint32_t)(nowv - L->last_present) >= present_gap_us()) {
                    g_mq.active = 0;
                    console_damage_reset();
                    nowplaying_render(player_track_name(), elapsed,
                                      player_total_s(), player_buf_pct());
                    if (toast) {
                        screen_battery_toast_render();
                        battwarn_toast_shown(nowv);   /* its 4 s start here */
                        g_mq.active = 0;      /* see the list branch below */
                    }
                    L->toast_prev = toast;
                    if (!want_full) {
                        /* Volume edge only. The screen was re-rendered in full
                         * (the plate's anti-aliased corners blend into whatever
                         * is under them, so a fresh background is the only way
                         * to draw them — or to erase them — without drift), but
                         * everything outside the plate came out as the pixels
                         * the panel already shows. Push just the plate rect;
                         * console.c's damage rect is a union, so replace it.
                         * np_last is left alone: if the second rolled over in
                         * this pass, the next one presents the transport band. */
                        console_damage_reset();
                        console_damage_add(VOL_PLATE_X, VOL_PLATE_Y,
                                           VOL_PLATE_W, VOL_PLATE_H);
                    }
                    ui_present_damage();
                    L->np_first = 0;
                    if (want_full) {
                        L->np_last = elapsed;
                        L->dirty   = 0;
                    }
                    L->np_vol_prev  = vol_active;
                    L->np_vol_dirty = 0;
                    L->last_present = nowv;
                    player_note_presented();
                    g_lp.valid = 0;             /* not a list screen */
                }
                /* else: throttled — keep dirty / the volume edge pending, present
                 * in the next window (np_vol_prev is what makes a fade retry). */
            } else if (elapsed != L->np_last) {
                /* Transport only: leave the marquee registered (its own tick
                 * keeps the title scrolling) and don't touch the art/metadata.
                 * The clock lands here once a second; the scrubber lands here
                 * on every detent (it forces np_last), so THAT case is paced by
                 * the last present's cost like every other wheel-driven paint —
                 * np_last stays forced until the band actually goes out. */
                /* Paced against the last present of ANY kind, scrubbing or
                 * not: the clock's band a few ms behind a full frame found the
                 * BCM still retiring it (see ui_present_damage). np_last stays
                 * unchanged when throttled, so the band goes out next pass. */
                if ((uint32_t)(nowv - L->last_present) >= present_gap_us()) {
                    console_damage_reset();
                    nowplaying_transport_render(elapsed, player_total_s());
                    ui_present_damage();
                    L->np_last      = elapsed;
                    L->last_present = nowv;
                    player_note_presented();
                }
            }
        } else if (L->dirty || az_letter != L->az_prev || toast != L->toast_prev) {
            /* Repaints are paced by what the LAST present actually cost (see
             * present_gap_us), not by a fixed 150 ms: a selection move now
             * repaints two rows and pushes only those, so it can run several
             * times faster without keeping IRQs masked long enough to starve the
             * audio DMA ISR. Idle → present immediately. `dirty` stays set until
             * we present, so the latest scroll position is what lands. */
            uint32_t now = mmio_read32(USEC_TIMER_ADDR);
            if (!player_active() ||
                (uint32_t)(now - L->last_present) >= present_gap_us()) {
                g_mq.active = 0;
                console_damage_reset();
                /* Selection moved and nothing else did? Repaint the two rows
                 * (plus the header count + scrollbar) instead of clearing and
                 * redrawing the panel. Anything else falls through to the full
                 * render, which console_clear marks as whole-screen damage. The
                 * A-Z plate covers arbitrary rows, so while it is up (or in the
                 * frame that clears it) the full render is the honest option. */
                int partial = !az_letter && !L->az_prev && !toast && !L->toast_prev &&
                              list_repaint_partial();
                if (!partial) {
                    switch (scr_cur()) {
                    case SCR_MENU:    main_menu_render();            break;
                    case SCR_MUSIC:   music_menu_render();           break;
                    case SCR_ARTISTS: artists_render(g_artist_sel);  break;
                    case SCR_SONGS:   songs_render(g_song_sel);      break;
                    case SCR_GENRES:  genres_render(g_genre_sel);    break;
                    case SCR_PLAYLISTS: playlists_render(g_pl_sel);  break;
                    case SCR_PLAYLIST:  playlist_render(g_plt_sel);  break;
                    case SCR_OTG:       otg_render(g_otg_sel);       break;
                    case SCR_SEARCH:    search_render_cur();         break;
                    case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel);   break;
                    case SCR_QUEUE:   queue_render(g_queue_sel);     break;
                    case SCR_SETTINGS: settings_render_cur();        break;
                    case SCR_BATTERY:
                        battery_modal_paint();
                        break;
                    case SCR_CHARGING:
                        charging_screen_paint();
                        break;
                    default: break;                 /* NOWPLAYING handled above */
                    }
                }
                if (az_letter) az_overlay_render(az_letter);
                if (toast) {
                    screen_battery_toast_render();
                    battwarn_toast_shown(now);        /* its 4 s start here */
                }
                /* The plate owns the screen. The repaint above re-registered
                 * the selected row with the marquee, whose tick would then
                 * punch a hole straight through whatever is covering it. */
                if (az_letter || toast) g_mq.active = 0;
                /* Render and present timed apart: `now` was read before the
                 * paint, so the delta here is the CPU the renderer took. */
                g_ui_render_us = mmio_read32(USEC_TIMER_ADDR) - now;
                if (g_ui_render_us > g_ui_render_max_us) g_ui_render_max_us = g_ui_render_us;
                ui_present_damage();
                if (g_present_cost_us > g_ui_present_max_us) g_ui_present_max_us = g_present_cost_us;
                if (partial) g_ui_partial++; else g_ui_full++;
                list_paint_note();
                L->az_prev = az_letter;
                L->toast_prev = toast;
                L->dirty = 0;
                L->last_present = now;
            }
        }

        /* Animate the now-playing 3-bar indicator in the album detail: redraw
         * only its ~10px box and partial-present just that, ~9fps. A tiny pixel
         * push (unlike a full repaint) so it can't starve the audio DMA. Not
         * while paused: the bars stand still (bars_clock) and nothing need be
         * pushed. */
        if (L->bl_state != BL_OFF && scr_cur() == SCR_BROWSER && g_dir_depth > 0
            && player_active() && !player_paused()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - L->last_bars) >= 110000u) {
                int sv = 0;
                for (int i = 0; i < g_det_view_n; i++)
                    if (g_det_view[i] == (int16_t)g_det_sel) { sv = i; break; }
                int top = ui_scroll_window(sv, g_det_view_n, DET_ROWS);
                const char *pn = player_track_name();
                for (int r = 0; r < DET_ROWS; r++) {
                    int vi = top + r;
                    if (vi >= g_det_view_n) break;
                    int16_t v = g_det_view[vi];
                    if (v < 0) continue;
                    const browse_entry_t *e = &g_browse[v];
                    if (e->is_dir || !name_eq_ci(e->name, pn)) continue;
                    int ry = DET_LIST_Y0 + r * ROW_H;
                    int is_sel = (v == g_det_sel);
                    console_fill_rect(15, ry + 6, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK,
                                    bars_clock());
                    lcd_present_rect(console_framebuffer(),
                                     15, ry + 6, NP_BARS_W + 1, NP_BARS_H);
                    break;
                }
                L->last_bars = nowb;
            }
        }

        /* Same animated bars on the queue view (the currently-playing row). */
        if (L->bl_state != BL_OFF && scr_cur() == SCR_QUEUE && player_active()
            && !player_paused()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - L->last_bars) >= 110000u) {
                int n = player_queue_len();
                int cur = player_queue_current();
                int top = ui_scroll_window(g_queue_sel, n, LIST_ROWS);
                int r = cur - top;
                if (cur < n && r >= 0 && r < LIST_ROWS) {
                    int ry = LIST_Y0 + r * ROW_H;
                    int bx = LCD_WIDTH - 22, by = ry + 7;
                    int is_sel = (cur == g_queue_sel);
                    console_fill_rect(bx, by, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(bx, by, is_sel ? LINEN_SEL_FG : LINEN_INK,
                                    bars_clock());
                    lcd_present_rect(console_framebuffer(),
                                     bx, by, NP_BARS_W + 1, NP_BARS_H);
                }
                L->last_bars = nowb;
            }
        }

        /* Scroll the marquee target (a selected long row, or the now-playing
         * title) in place — redraw just its band + partial-present it, ~16fps. */
        if (L->bl_state != BL_OFF && g_mq.active) {
            uint32_t nowm = mmio_read32(USEC_TIMER_ADDR);
            /* Phase clock (g_mq.t0) + target-change detection live in mq_set, so
             * this just paces the redraw at the shared live offset. */
            if ((uint32_t)(nowm - L->last_mq) >= 33000u) {   /* ~30fps               */
                draw_marquee(g_mq.x, g_mq.y, g_mq.w, g_mq.text, g_mq.tw, g_mq.font,
                             g_mq.ink, g_mq.bg, nowm - g_mq.t0, g_mq.cy0, g_mq.cy1);
                int ptop = g_mq.y - text_ascent(g_mq.font);
                int pbot = g_mq.y + text_descent(g_mq.font);
                if (ptop < 0) ptop = 0;
                lcd_present_rect(console_framebuffer(),
                                 g_mq.x, ptop, g_mq.w, pbot - ptop);
                L->last_mq = nowm;
            }
        }

        /* Load album covers while on the album list: the budget rule is
         * app/coverpump.c, the reads albumlist_pump_chips (app/albums.c). */
        if (scr_cur() == SCR_BROWSER && g_dir_depth == 0) {
            uint32_t rows = albumlist_pump_chips(fs, g_locked, &L->last_chip);
            if (rows) {
                g_lp.rows |= rows;
                L->dirty = 1;
            }
        }

        /* The scroll diagnostics line, on the same 5 s cadence as the battery
         * and audio lines but only when something was painted. */
        {
            uint32_t nows = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nows - L->last_uistat) >= 5000000u) {
                ui_stats_emit();
                L->last_uistat = nows;
            }
        }

        /* While PLAYING, a pass that did nothing — no decode work, no repaint —
         * used to free-spin the core at 80 MHz, re-polling the wheel (which masks
         * and unmasks IRQs) thousands of times a second. Halt instead. The halt
         * is 200 us, NOT milliseconds: the countdown is not documented to wake on
         * an interrupt, and the audio DMA ISR has only the ~363 us I2S FIFO of
         * slack — 200 us stays inside that budget while still parking the core
         * for the overwhelming majority of an idle pass. Input is latched by the
         * tick ISR and every animation here ticks at >= 33 ms, so nothing is lost.
         *
         * Gated on player_playing(), not player_active(). The 200 us figure is a
         * DMA-feeding budget, and a PAUSED player has no DMA to feed — but
         * active() stays 1 across a pause, so a paused device used to take this
         * branch and spin the loop ~5,000 times a second (each pass a
         * player_pump call, several USEC_TIMER reads, GPIO reads and an event
         * drain) for as long as it sat paused. Paused is idle: it belongs in
         * the 10 ms halt below, fifty times fewer wakeups. */
        if (player_playing() && !L->dirty && pump_us < 200u) {
            cpu_wait_us(200);
        }

        /* Only throttle when idle; while playing, player_pump's decode_step
         * paces the loop and the wheel stays responsive. Halt the core until the
         * next tick (self-waking ~10 ms) instead of a busy-spin: input is latched
         * by the 100 Hz timer ISR, so this costs no responsiveness. "Idle"
         * includes paused (see above); the paused pump's only job — the codec
         * power-down timeout — is seconds long, so 100 Hz is ample for it. */
        if (!player_playing()) {
            cpu_wait_ms(10);
        }

        /*
         * Supervisor stack guard. crt0 paints the region below _stack_limit, so
         * this is an O(1) look at the low words — cheap enough to run every idle
         * pass. A breach means a deep UI call chain has already written past the
         * bottom of the stack, which is silent memory corruption; panic() at
         * least turns it into a readable screen with the faulting PC instead of
         * a mystery freeze or garbled state hours later.
         */
        if (stack_guard_breached()) {
            panic(PANIC_STACK, 0);
        }
    }
}

/* Per-task stack for the idle task (carved from .bss). Only reached if the LCD
 * is unpowered or the disk won't mount — the browser above never returns. */
#define TASK_STACK_SIZE 1024
static uint8_t idle_stack[TASK_STACK_SIZE];

/* Disk scratch (uint16_t for the 2-byte alignment ata_read_sectors needs). */
static uint16_t mbr_sector[1024];

/*
 * Idle task: never exits. Sleep the CPU for a short countdown, then yield.
 * The scheduler is only started on the no-LCD / mount-failure fallback path.
 */
_Noreturn static void idle_task(void) {
    uart_puts("core: idle task entered\n");
    for (;;) {
        cpu_wait_ms(10);
        sched_yield();
    }
}

_Noreturn void kernel_main(void) {
    /* Free-running 1 MHz microsecond counter (USEC_TIMER, 01-soc-pp5022.md
     * "Timers"), sampled at the very top so later milestones can report
     * elapsed-since-boot in real time, INDEPENDENT of the 100 Hz tick. */
    uint32_t boot_us0 = mmio_read32(USEC_TIMER_ADDR);

    uart_init();

    uart_puts("core: kernel alive (iPod 5G/5.5G, PP5022)\n");
    /* The build, said by the build itself. Every byte the UART sends is also
     * captured into the event log (evlog_capture), so once this reaches the
     * disk the log can name which firmware a boot ran — the host otherwise
     * only knows what it WROTE (COREFLSH.TXT) and infers the rest. A plain
     * string, deliberately without the CORE-FW-VERSION: prefix: the image
     * must carry exactly one marker for check_version_marker.sh to find. */
    uart_puts("core: version " CORE_VERSION " build " CORE_BUILD_ID "\n");

    /*
     * Hand the shared list chrome its scrolling-title implementation.
     *
     * ui_list_row() draws an overflowing title through this hook. The marquee
     * needs USEC_TIMER, and a time source is exactly what ui/chrome.c must not
     * reach for if it is to build on the host — so the dependency is injected
     * here instead. Registered once, before anything can paint; leave it unset
     * (as the host tests do) and titles are plainly clipped.
     */
    ui_set_scroll_text(mq_text);

    /*
     * The wheel's acceleration state machine (ui/wheel.c) is portable for the
     * same reason and by the same means: its clock, its "what letter is row N
     * on this screen" source and its navigation click are injected here rather
     * than reached for. Unset, none of them can misbehave — no clock reads 0,
     * no letter source means nothing letter-steps, no click is silent.
     */
    wheel_set_clock(wheel_clock);
    wheel_set_initial_at(list_initial_at);
    wheel_set_letter_step(list_letter_step_idx);
    /* .bss zeros happen to be Search's start state (PICK, the cursor on 'A',
     * an empty query); saying so means a field added to search_t later cannot
     * quietly start as something else. */
    search_reset(&g_search);
    wheel_set_click(ui_click);

    /* Hex-path self-test: if this doesn't read 1234ABCD on the terminal,
     * distrust every register dump that follows. */
    uart_puts("core: uart self-test ");
    uart_put_hex32(0x1234ABCD);
    uart_putc('\n');

    uart_puts("core: PROCESSOR_ID ");
    uart_put_hex32(mmio_read32(PROCESSOR_ID_ADDR));   /* through the seam: same load on hw */
    uart_putc('\n');

    /* Come off the 24 MHz boot clock up to CPUFREQ_NORMAL (30 MHz) before the
     * rest of bring-up so it runs at a sane speed. */
    uart_puts("core: clock init -> 30 MHz\n");
    clock_init();
    uart_puts("core: cpu freq ");
    uart_put_hex32(cpu_frequency());
    uart_putc('\n');

    /* Turn on the unified cache now — decode is far too slow with it off.
     * Write-back, so the audio DMA path flushes (cache_commit) before the DMA
     * reads a freshly-filled buffer. */
    uart_puts("core: cache init\n");
    cache_init();

    /* 100 Hz system tick + core IRQ unmask BEFORE audio: continuous playback is
     * DMA-driven and needs its completion interrupt delivered, and sleep_ms
     * spins on the IRQ-fed tick. */
    uart_puts("core: timer init @ 100 Hz, enabling IRQs\n");
    timer_init();
    arch_irq_enable();

    /* Boot clock starts here — the earliest point with a running timer. */
    g_boot_t0_us = mmio_read32(USEC_TIMER_ADDR);

    /* (A tick self-test used to sleep 100 ms here to prove the IRQ-fed tick
     * advanced. It did its job during bring-up; on a cold boot it is 100 ms of
     * a 10 s startup spent proving something the scheduler exercises anyway.) */

    /*
     * Backlight FIRST — before the LCD probe, deliberately.
     *
     * It used to live inside the lcd_init() block, which made it invisible
     * whether a dark panel meant "our code never ran" or "our code ran and
     * lcd_init() failed". Chainloaded from ipodloader2 that never mattered:
     * the loader draws its own menu, so it has already lit the backlight and
     * brought the BCM up before handing off. Booting DIRECTLY from the
     * firmware partition, nobody has — and a cold-BCM lcd_init() failure
     * skipped the backlight, the disk and the UI in one go, which is
     * indistinguishable from a crash in crt0 (2026-07-26: black screen, cause
     * undiagnosable for exactly this reason).
     *
     * Nothing here depends on the LCD: backlight_init() is GPIO B/D/L only
     * (charge pump, step line, LED enable) and its delay loops self-scale to
     * the live core clock via cpu_frequency(), whose 30 MHz calibration point
     * is precisely where we are now — before cpu_boost(), not after.
     *
     * Cost: the panel is lit for the duration of lcd_init() (up to ~500 ms of
     * BCM bring-up) before the boot screen paints, so a cold boot can show a
     * brief bright field. That is the price of being able to see a boot at
     * all, and it is one line to put back.
     */
    backlight_init();

    /* LCD probe gates the whole disk/audio/UI stack: nonzero BCM power => real
     * hardware. The clicky emulator has no BCM (lcd_init() false there), so
     * this block is skipped, keeping the emulator smoke green; the register
     * grammar is proven host-side by the mock-bus trace tests. */
    uint32_t lcd_t0 = boot_ms_now();
    int      lcd_ok = lcd_init();
    g_boot_lcd_ms   = boot_ms_now() - lcd_t0;
    if (lcd_ok) {
        /* Boost to 80 MHz for the whole disk/decode path. The UI runs here too;
         * we never drop back because the browser never returns (fine for
         * bring-up — the device is on a cable during testing). */
        cpu_boost();

        /* Bring up the I2C control bus now (not just at first-song hal_audio_init)
         * so the status strip can read the PCF50605 battery gauge from the menu
         * — and so run_ui's boot block can read the same chip's clock, which it
         * does before anything paints a time. Bounded/idempotent —
         * hal_audio_init re-inits it harmlessly per track. */
        i2c_init();
        battery_init();
        hal_volume_init();               /* codec output gain -> safe default      */
        piezo_init();                    /* PWM click for menu navigation          */
        hal_audio_boot_quiet();          /* codec cold + muted until the first     *
                                          * track: a Menu+Select reset keeps its   *
                                          * rails up, and an inherited live codec  *
                                          * hisses on the jack with nothing playing */

        /* Paint the boot screen immediately, so the panel shows CORE branding
         * instead of whatever the panel held while the disk spins up and the
         * volume mounts. No bar yet — nothing measurable has started. This one
         * paint is necessarily the default palette; run_ui repaints it in the
         * user's theme as soon as the settings are read. */
        boot_screen_render("LOADING", -1);

        /* Read the MBR, find the FAT32 data partition (type 0B/0C), mount it. */
        uint8_t *mbr = (uint8_t *)mbr_sector;
        uint32_t sig = 0, fat_lba = 0;
        int      mnt = -1;
        fat32_t  fs;
        /* Everything from here to the end of fat32_mount is "the disk": the
         * IDE reset, the first read (which is what actually spins the platters
         * up, and is allowed 4 s for it — see ATA_SPINUP_US), and the mount's
         * own reads. Timed as one phase because that is the unit we could move
         * or overlap; splitting it finer would not change what we can do. */
        uint32_t disk_t0 = boot_ms_now();
        if (ata_init() == 0 && ata_read_sectors(0, 1, mbr) == 0) {
            sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
            for (int p = 0; p < 4; p++) {
                const uint8_t *e = &mbr[0x1BE + 16 * p];
                if (e[4] == 0x0B || e[4] == 0x0C) {
                    fat_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                              ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                    break;
                }
            }
        }
        if (sig == 0xAA55u && fat_lba != 0) {
            /* The MBR start-LBA may be in native 512-byte units OR (stock 80 GB,
             * 2048-byte FAT sectors) in 2048-byte units. Try as-is, then x4;
             * fat32_mount validates the boot signature + BPB. */
            mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba);
            if (mnt != 0) {
                mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba * 4u);
            }
        }

        g_boot_disk_ms = boot_ms_now() - disk_t0;

        uint32_t btus = mmio_read32(USEC_TIMER_ADDR) - boot_us0;
        uart_puts("core: mount rc ");
        uart_put_hex32((uint32_t)mnt);
        uart_puts(" BTUS ");
        uart_put_hex32(btus);
        uart_putc('\n');

        if (mnt == 0) {
            /* Capacity / free (MB) for the About screen (64-bit to avoid the
             * clusters*bytes overflow on a big volume). */
            g_total_mb = (uint32_t)(((uint64_t)fs.total_clus * fs.clus_bytes) >> 20);
            g_free_mb  = (fs.free_clus == 0xFFFFFFFFu) ? 0xFFFFFFFFu
                       : (uint32_t)(((uint64_t)fs.free_clus * fs.clus_bytes) >> 20);
            uart_puts("core: entering browser\n");
            run_ui(&fs);                       /* never returns */
        }

        /* Mount failed: show a diagnostic error screen and fall through to the
         * idle scheduler so the device isn't a black brick. */
        console_clear(CON_RED);
        console_str  (2, 3, "MOUNT FAILED", CON_WHITE, CON_RED);
        console_str  (2, 6, "SIG",  CON_WHITE, CON_RED);
        console_hex32(8, 6, sig,               CON_WHITE, CON_RED);
        console_str  (2, 8, "MNT",  CON_WHITE, CON_RED);
        console_hex32(8, 8, (uint32_t)mnt,     CON_WHITE, CON_RED);
        console_str  (2, 10, "PART", CON_WHITE, CON_RED);
        console_hex32(8, 10, fat_lba,          CON_WHITE, CON_RED);
        lcd_present_fb(console_framebuffer());
        cpu_unboost();
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping disk + UI\n");
    }

    /* Fallback only (no LCD or mount failure): idle the core under the
     * cooperative scheduler. sched_start never returns. */
    uart_puts("core: sched init (idle fallback)\n");
    sched_init();
    if (sched_add_task(idle_task, idle_stack, sizeof idle_stack, "idle") < 0) {
        uart_puts("core: FATAL sched_add_task failed\n");
        for (;;) {
        }
    }
    sched_start();
}
