/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/settings_glue.c — see settings_glue.h.
 */

#include <stdint.h>

#include "hal.h"
#include "hw/mmio.h"
#include "hw/pp5022.h"
#include "hw/uart.h"
#include "hw/ata.h"
#include "hw/battery.h"
#include "hw/headphone.h"
#include "hw/volume.h"
#include "../player/player.h"
#include "../kernel/config.h"
#include "../kernel/evlog.h"
#include "../kernel/chargestat.h"
#include "core_version.h"      /* CORE_BUILD_ID: meson vcs_tag, git describe   */
#include "core_version_tag.h"  /* CORE_VERSION:  meson vcs_tag, nearest tag    */
#include "../ui/settings.h"
#include "../ui/eq.h"
#include "../ui/palette.h"
#include "../ui/sleeptimer.h"
#include "../ui/settime.h"
#include "state.h"
#include "clockui.h"
#include "persist.h"
#include "nav.h"
#include "library.h"
#include "battery_ui.h"
#include "albums.h"
#include "statusbar.h"
#include "main_internal.h"
#include "resume.h"
#include "settings_glue.h"

/* ---------------------------------------------------------------------------
 * Settings (core/ui/settings.h data-driven model). g_settings holds the state;
 * g_set_screen tracks the current sub-screen within the one SCR_SETTINGS stack
 * entry. Slider rows use a brief edit mode (SELECT toggles it, then the wheel
 * adjusts) so the wheel can still move the selection otherwise.
 * ------------------------------------------------------------------------- */
int g_set_screen;                  /* current settings_screen_t          */
int g_set_sel;                     /* selection within g_set_screen      */
int g_set_root_sel;                /* saved ROOT selection               */
int g_set_accum;                   /* wheel accumulator                  */
int g_set_editing;                 /* editing a slider row               */

/* The UI's band count and the HAL's are the same silicon's; a curve is copied
 * across the boundary as a plain array, so a disagreement would be a buffer
 * overrun rather than a compile error. */
_Static_assert(EQ_BANDS == EQ_BAND_COUNT, "ui/eq.h and wm8758.h disagree");

/* Push the FUNCTIONAL settings out to the subsystems. Cosmetic fields are a
 * no-op. The backlight timeout/brightness are read live by the loop. */
void settings_apply(void)
{
    player_set_shuffle(g_settings.shuffle);
    player_set_repeat((int)g_settings.repeat);
    /* The volume ceiling goes through the same helper the Now Playing wheel
     * uses. config_decode and settings_adjust both keep volume <= limit, so
     * this only ever bites on a record from somewhere else -- and then it
     * writes the clamped value back, rather than leaving the Settings slider
     * showing a level the codec is not being given. */
    g_volume = settings_volume_clamp(&g_settings, g_settings.volume);
    g_settings.volume = g_volume;
    hal_volume_set(g_volume);
    hal_balance_set(g_settings.balance);
    /* A preset owns both shelves; at EQ Off it is the Bass/Treble sliders. */
    eq_curve_t curve;
    eq_effective_curve(g_settings.eq, g_settings.bass, g_settings.treble,
                       &curve);
    hal_eq_set(curve.gain_db, curve.cutoff, curve.narrow);
    theme_set(g_settings.theme);           /* Linen / Onyx -> live palette swap */
    chip_placeholder_init();               /* re-bake LINEN_BORDER: the album
                                            * list's empty chips follow a real
                                            * theme change (a preview does not
                                            * reach here — theme_preview_sync) */
    charger_apply();                       /* Charge Rate -> LTC4066 HPWR       */
}

/*
 * Bring the running countdown in step with the Settings row (the invariant at
 * g_sleep). Called after anything that MOVES g_settings.sleep_timer_min: the
 * row's SELECT and Reset Settings. Deliberately NOT called from the boot path
 * — .bss and config_decode both leave the field at 0, so there is nothing to
 * arm, and calling it there would be the one place a stale record could.
 *
 * The guard is on the VALUE, so an apply that finds the two sides already in
 * step does nothing and cannot restart a countdown by accident. The row's
 * SELECT always moves the value (it cycles), so every press does re-arm —
 * including the sixth, which lands back where it started and is plainly meant
 * as a fresh start.
 */
void sleep_timer_apply(void)
{
    if (g_settings.sleep_timer_min == sleeptimer_total_min(&g_sleep)) {
        return;
    }
    sleeptimer_arm(&g_sleep, g_settings.sleep_timer_min,
                   mmio_read32(USEC_TIMER_ADDR));
    if (g_settings.sleep_timer_min > 0) {
        uart_puts("core: sleep timer: armed ");
        uart_dec(g_settings.sleep_timer_min);
        uart_puts(" min\n");
    } else {
        uart_puts("core: sleep timer: off\n");
    }
}

/* Every Settings screen gets the same status strip as the lists: the painters
 * in ui/screen_settings.c leave rows 0..STATUS_H-1 clear (their console_clear
 * is what puts the surface there), so the strip is painted over that band here,
 * after the painter returns. */
void settings_render_cur(void)
{
    /* Date & Time's first row shows the time it would edit, and ui/settings.c
     * is pure — it has no clock. Injected once per paint, the way the marquee's
     * clock is injected into ui/chrome.c. */
    {
        uint32_t local = 0;
        settings_set_now(clock_local_epoch(&local), local);
    }

    if (g_set_screen == SETTINGS_ABOUT) {
        /* The counts are capped (LIB_MAX_SONGS/ALBUMS, ARTISTS_MAX,
         * LIB_MAX_GENRES). When a load actually hit one of those caps the
         * screen says so — otherwise a library that's too big just looks
         * like it lost tracks. The warning and the load time are
         * INDEPENDENT (the warning used to hide the number entirely). */
        /* The jack token is the only probe this device has for the detect
         * line (ui/settings.h). Three reads, paid on this page alone: the
         * pin and its two configuration registers. The debounced half comes
         * from the loop's own last answer, so it reads -1 — and the token
         * drops that half — exactly while the line is untrusted. */
        about_jack_t jack = {
            .raw       = (int8_t)headphone_raw(),
            .debounced = g_jack.last,
            .pin_cfg   = (uint8_t)headphone_pin_cfg(),
            .edges     = g_jack.raw_edges,
        };
        settings_about_render(g_bat_pct, g_bat_mv, g_bat_raw, g_total_mb, g_free_mb,
                              g_songs_n, g_albums_n, g_artists_n,
                              evlog_seq(),
                              evlog_enabled()  ? ABOUT_LOG_ON :
                              evlog_failures() ? ABOUT_LOG_ERR : ABOUT_LOG_OFF,
                              g_lib_truncated, CORE_VERSION, &jack);
    } else if (g_set_screen == SETTINGS_DIAG) {
        /* Boot Details owns the diagnostics now: the cold-boot phase
         * breakdown and the settings-file locator. They used to be squeezed
         * into About's spare gaps, which is how the CFG rows ended up on top
         * of the stat columns. Read-only.
         *
         * The LBAs resolve FRESH here — exactly what a save would target,
         * rather than something cached — but ONLY while the drive is already
         * spinning, because every probe walks the FAT chain and this page is
         * repainted on every frame. With the drive parked, opening the page
         * paid a 1-3 s spin-up before its first pixel; it now shows the pair
         * probed at boot, which is the same address unless the file moved (it
         * does not — the host pre-allocates it). Each probe zeroes its
         * out-param before it can fail, so they land in locals and are copied
         * back only on success: a transient read error must not blank a good
         * boot-time value. The log's "next" LBA advances as blocks flush, so
         * the parked reading can lag by a few blocks — acceptable, since the
         * cross-check against make_log.py --verify is done with the device on
         * the cable and the drive up. */
        if (!ata_is_parked()) {
            uint32_t p0 = 0, p1 = 0;
            if (config_probe_lba(0, &p0) == 0) g_diag_cfg_lba[0] = p0;
            if (config_probe_lba(1, &p1) == 0) g_diag_cfg_lba[1] = p1;
            if (evlog_enabled()) {
                uint32_t h = 0, n = 0;
                if (evlog_probe_header_lba(&h) == 0)          g_diag_log_lba[0] = h;
                if (evlog_probe_lba(evlog_seq(), &n) == 0)    g_diag_log_lba[1] = n;
            }
        }
        const player_stats_t *ps = player_stats();
        settings_diag_render(g_boot_total_ms, g_boot_lcd_ms, g_boot_disk_ms,
                             g_lib_load_ms, g_boot_resume_ms,
                             g_boot_res_dir_ms, g_boot_res_open_ms,
                             g_boot_res_seek_ms,
                             ps ? ps->decode_us_per_kframe : 0,
                             ps ? ps->decode_rate : 0,
                             ps ? ps->underruns : 0,
                             config_writable(), config_seq(),
                             g_diag_cfg_lba[0], g_diag_cfg_lba[1],
                             g_diag_log_lba[0], g_diag_log_lba[1],
                             CORE_BUILD_ID);
    } else if (g_set_screen == SETTINGS_BATTERY) {
        /* The Battery page: everything on it but the Charge Rate row is a
         * live value, and this is the only place that has them. The pins
         * are read back per paint (six GPIO reads, no I2C) — the point of
         * the footer is that it shows the pad, not the request. The trend
         * comes from the record every 5 s sample feeds (chargestat). */
        charger_pins_t pins;
        charger_pin_state(&pins);
        static uint16_t hist[BATTERY_HIST_MAX];
        battery_page_t bp;
        uint32_t now = mmio_read32(USEC_TIMER_ADDR);
        bp.pct         = g_bat_pct;
        bp.mv          = g_bat_mv_filt;
        bp.adc         = g_bat_raw;
        bp.source      = power_source();
        bp.charging    = power_is_charging();
        bp.rate_ma     = charger_max_current();
        bp.hpwr        = pins.hpwr;
        bp.susp        = pins.susp;
        bp.hpwr_cfg    = (uint8_t)((pins.hpwr_gpio ? BATTERY_PIN_GPIO : 0) |
                                   (pins.hpwr_out  ? BATTERY_PIN_OUT  : 0));
        bp.susp_cfg    = (uint8_t)((pins.susp_gpio ? BATTERY_PIN_GPIO : 0) |
                                   (pins.susp_out  ? BATTERY_PIN_OUT  : 0));
        bp.session_s   = chargestat_session_s(&g_chg, now);
        bp.session_mv0 = chargestat_session_mv0(&g_chg);
        bp.session_dmv = chargestat_session_delta_mv(&g_chg);
        bp.session_chg = chargestat_session_chg_pct(&g_chg);
        bp.trend_ok    = chargestat_recent_delta(&g_chg, BATTERY_TREND_MIN,
                                                 &bp.trend_mv);
        if (!bp.trend_ok) bp.trend_mv = 0;
        bp.hist_n      = chargestat_history(&g_chg, hist);
        bp.hist        = hist;
        bp.playing     = player_active() && !player_paused();
        bp.parked      = ata_is_parked();
        settings_battery_render(&g_settings, g_set_sel, &bp);
    } else if (g_set_screen == SETTINGS_SETTIME) {
        settime_render(&g_settime);
    } else {
        settings_render(g_set_screen, &g_settings, g_set_sel);
    }
    status_strip_render();
}

/*
 * Leaving Settings: the bookkeeping that MENU does on the way out, without
 * the pop itself. Extracted so the MENU tap and the MENU hold (which jumps
 * straight to the main menu from anywhere) run exactly the same exit — a
 * change made in Settings has to reach the platter either way.
 *
 * Resume may have just been switched OFF, and dropping the stored locator is
 * part of honouring that; doing it here folds the clear into the commit
 * instead of costing a second write when the interval next comes round.
 *
 * SOFT, not FORCE. resume_capture() marks the record dirty whenever a track
 * is loaded (the position moved), so a FORCE here meant that leaving Settings
 * with the drive PARKED paid a 1-3 s ata_wakeup() spin-up before the pop was
 * even rendered — the stall was the back-out itself, every time. SOFT writes
 * when the platters are already turning and otherwise leaves the change
 * pending for the idle path or the next forced commit (suspend / power-off /
 * disk mode).
 */
void settings_leave(void)
{
    g_set_editing = 0;
    resume_capture();
    settings_commit(CFG_COMMIT_SOFT);
    otg_commit(CFG_COMMIT_SOFT);          /* a list built before Settings, too */
}

/*
 * Settings > Theme previews the theme under the cursor: the picker page is
 * painted in it as the wheel moves, and nothing in g_settings changes, so
 * nothing is written. SELECT applies for real (settings_activate ->
 * settings_apply); every other way out — MENU back to the Settings root, the
 * MENU hold to the main menu, a modal pushed over it, a suspend — lands on a
 * screen where settings_theme_shown() answers the saved theme, and the next
 * pass puts it back. Called once per main-loop pass, after input and before
 * the repaint; returns 1 when it swapped the palette (the caller repaints the
 * whole panel). theme_current() makes the steady state free: no copy unless
 * the wanted theme changed.
 *
 * The chip placeholders are deliberately NOT re-baked for a preview: no
 * Settings screen draws an album chip, and the only screens that do are
 * reached through a path where the saved theme is already back.
 */
int theme_preview_sync(void)
{
    int want = (scr_cur() == SCR_SETTINGS)
                   ? settings_theme_shown(g_set_screen, g_set_sel, &g_settings)
                   : g_settings.theme;
    if (want == theme_current()) return 0;
    theme_set(want);
    return 1;
}
