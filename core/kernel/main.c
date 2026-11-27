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
#include "../app/gestures.h"
#include "../app/power.h"
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
                /* The pull is an input, like the Hold edge: it lights the
                 * screen to show the pause. Left dark (a long listen with no
                 * input lets it go out), the first PLAY after plugging back
                 * in was only the wake press, and resuming took two. */
                L->last_input = mmio_read32(USEC_TIMER_ADDR);
                if (L->bl_state != BL_FULL) {
                    if (!L->panel_slept) backlight_set(g_settings.backlight_bright);
                    L->bl_state = BL_FULL;
                    wheel_accel_reset();
                }
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
        gesture_seek_menu(L);

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
        gesture_hold_switch(L);

        /* Input is sampled on the 100 Hz tick and latched (clickwheel_service),
         * so a tap that lands while this loop is blocked in a disk read isn't
         * lost — we just drain the latch here. */
        wheel_event_t ev;
        int have_ev = clickwheel_get_event(&ev) ? 1 : 0;
        have_ev = gesture_input_gate(L, &ev, have_ev);
        if (have_ev) {
            L->last_input = mmio_read32(USEC_TIMER_ADDR);
            /* The tick the event is timed from: a row press's length is
             * measured from HERE, not from the first pass the live sampler
             * happens to see the button down on. */
            const uint32_t ev_us = L->last_input;
            gesture_input_front(L, &ev);
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
                playlists_input(&ev, L, fs, ev_us);
                break;

            case SCR_PLAYLIST:
                playlist_input(&ev, L, fs, ev_us);
                break;

            case SCR_OTG:
                otg_input(&ev, L, fs, ev_us);
                break;

            case SCR_SEARCH:
                search_input(&ev, L, fs, ev_us);
                break;

            case SCR_BROWSER:
                browse_input(&ev, L, fs, ev_us);
                break;

            case SCR_NOWPLAYING:
                nowplaying_input(&ev, L, fs, ev_us);
                break;

            case SCR_QUEUE:
                queue_input(&ev, L, fs, ev_us);
                break;

            case SCR_SETTINGS:
                settings_input(&ev, L, fs, ev_us);
                break;

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
        gesture_row_select(L, fs);

        /*
         * SELECT press-length arbitration (Now Playing). Decided here rather
         * than at the down-edge because the length is only known once the button
         * is released or the hold threshold passes. Held past SEL_HOLD_US opens
         * the queue immediately (so the gesture confirms itself under your
         * thumb); released before it toggles the scrubber.
         */
        gesture_np_select(L);

        /*
         * Deferred seek. The target is committed once the wheel has been quiet
         * for SCRUB_COMMIT_US, so dragging across a track costs ONE seek rather
         * than one per detent — a seek stops the DAC, re-primes the ring, and on
         * a backward jump rewinds the file. Scrub mode then lapses on its own,
         * so the wheel cannot be left silently controlling position.
         */
        gesture_np_scrub(L);

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
         * which case the whole frame goes, as it always did. The block is
         * top_banner_tick (app/statusbar.c); `pump_us` is this pass's, for
         * the banner's own halt. */
        if (top_banner_tick(L, pump_us)) {
            continue;                     /* skip the normal render this pass      */
        }

        /* Render the current screen, or the parts of it that changed, and
         * present them, paced by the last present's cost: render_tick
         * (app/paint.c). */
        render_tick(L);

        /* Animate the now-playing 3-bar indicator in the album detail: redraw
         * only its ~10px box and partial-present just that, ~9fps. A tiny pixel
         * push (unlike a full repaint) so it can't starve the audio DMA. Not
         * while paused: the bars stand still (bars_clock) and nothing need be
         * pushed. */
        if (L->bl_state != BL_OFF && scr_cur() == SCR_BROWSER && g_dir_depth > 0
            && player_active() && !player_paused()) {
            detail_bars_tick(L);
        }

        /* Same animated bars on the queue view (the currently-playing row). */
        if (L->bl_state != BL_OFF && scr_cur() == SCR_QUEUE && player_active()
            && !player_paused()) {
            queue_bars_tick(L);
        }

        /* Scroll the marquee target (a selected long row, or the now-playing
         * title) in place — redraw just its band + partial-present it, ~30fps. */
        if (L->bl_state != BL_OFF && g_mq.active) {
            marquee_tick(L);
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
