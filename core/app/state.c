/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/state.c — the app-wide singletons; see state.h.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "state.h"

/*
 * Boot phase breakdown, all milliseconds, all shown on the About screen. The
 * library load already had a number; the other 8-ish seconds of a cold boot
 * did not, so "make it boot faster" had nowhere to aim. Same reasoning as
 * g_lib_load_ms: a number on the panel is what makes a change here verifiable
 * instead of a matter of impression.
 *
 * g_boot_t0_us is stamped once, as early as the timer allows, and every phase
 * is measured against the live USEC_TIMER rather than accumulated, so a phase
 * we forget to instrument shows up as the gap in the total rather than
 * silently vanishing.
 */
uint32_t     g_boot_t0_us;    /* stamped right after timer_init        */
uint32_t     g_boot_lcd_ms;   /* lcd_init(): cold BCM bring-up         */
uint32_t     g_boot_disk_ms;  /* ata_init + first read + FAT32 mount   */
uint32_t     g_boot_resume_ms;/* resume_restore(): readdir + open+seek */
uint32_t     g_boot_res_dir_ms  = BOOT_MS_NA; /* album readdir        */
uint32_t     g_boot_res_open_ms = BOOT_MS_NA; /* open + art + prime   */
uint32_t     g_boot_res_seek_ms = BOOT_MS_NA; /* player_seek_to()     */
uint32_t     g_boot_total_ms; /* to the first UI paint                 */

/* Boot Details' locator: CORECFG.DAT's two slot LBAs and the event log's
 * header / next-flush LBAs, as probed at boot with the drive spinning. The
 * page re-probes only while the drive is up: config_probe_lba and the evlog
 * probes walk the FAT chain (disk reads), so opening the page with the drive
 * parked used to spin it up for 1-3 s before its first paint. */
uint32_t     g_diag_cfg_lba[2], g_diag_log_lba[2];

/* ms since g_boot_t0_us (USEC_TIMER wraps; the subtraction is unsigned so a
 * wrap mid-boot still yields the right delta). */
uint32_t boot_ms_now(void)
{
    return (mmio_read32(USEC_TIMER_ADDR) - g_boot_t0_us) / 1000u;
}

/* Output volume (WM8758 codec gain, 0..100) + how long the on-screen volume
 * overlay stays up after the last wheel tick (volume-demo.jsx: ~1.5 s then fade).
 * The window is START-stamped, not deadline-stamped — see ui_window_t. */
int         g_volume = 70;
ui_window_t g_vol_show;

/* The live Settings state (ui/settings.h model). Declared up here because the
 * library/playback helpers below read it (shuffle, clicker); the navigation and
 * rendering that own it live in the Settings section further down. */
settings_t g_settings;

/*
 * The sleep timer's running countdown (ui/sleeptimer.h). Two sides, one
 * truth: g_settings.sleep_timer_min is what the Settings row SHOWS, g_sleep
 * is what RUNS, and the INVARIANT is
 *
 *     sleeptimer_total_min(&g_sleep) == g_settings.sleep_timer_min
 *
 * at every top of the main loop. Only sleep_timer_apply() below moves the
 * right side to match the left. The FIRE branch only disarms the countdown;
 * it is the shared suspend block, which every FIRE lands in, that zeroes the
 * row as well, so both sides are 0 by the next pass. .bss starts it disarmed, and
 * nothing in the boot path arms it, so the invariant holds from the first
 * pass (config_decode zeroes the field for exactly that reason).
 */
sleeptimer_t g_sleep;
/* The idle-sleep policy (kernel/idlesleep.h): nothing playing and no input
 * for two minutes -> the same suspend a PLAY hold enters, and so its
 * 30-minute escalation to a PMU standby. Device, 2026-09-24: a paused iPod
 * sat awake for twelve hours at ~35 mA because nothing ever took it out of
 * this loop. Fed once per pass next to the sleep timer; latched off for the
 * session by a refused standby. */
idlesleep_t  g_idlesleep;

/* Hold-switch lock state. While g_locked, all wheel/button input is swallowed
 * (playback keeps running); a brief banner takes the top chrome on the
 * engage/disengage edge and on a refused button press (lock_banner_render),
 * and a small padlock stays in the status strip while held. The dark modals
 * (charging, low battery) have no chrome and get neither: only the corner
 * padlock, painted with the screen (banner_hosted). */
int         g_locked;

/* Pause-on-unplug (ui/jackwatch.c): the believed jack level, the raw-edge
 * count the About footer shows, and whether this module owns the last pause.
 * Reset in run_ui() beside the PLAY keyhold — zero is not its "nothing known
 * yet" state. */
jackwatch_t g_jack;
ui_window_t g_lock_flash;

/*
 * THE CLOCK (kernel/wallclock.h, kernel/datetime.h, hal/hw/rtc.h).
 *
 * g_wclock is the software clock: anchored from the PMIC's RTC at boot, after
 * a manual set, after a suspend wake and every RTC_RESYNC_S, and carried in
 * between on the same USEC_TIMER everything else here is timed by — so the
 * displayed time costs no I2C traffic per frame. g_clock_min is the last
 * minute painted, which is the whole repaint rule: one comparison per pass.
 *
 * g_settime is the Date & Time editor's model while that screen is up.
 */
wallclock_t g_wclock;
settime_t   g_settime;
uint32_t    g_clock_min;          /* last painted minute; 0 = none yet */
uint32_t    g_clock_tick_us;      /* when the software clock last ticked */
uint32_t    g_clock_resync_us;    /* when it was last anchored from the chip */
int         g_clock_retry;        /* the last read got no answer: ask again soon */

/* Volume capacity / free (MB), computed once from the FS after mount for the
 * About screen. g_free_mb == 0xFFFFFFFF means the FSInfo free count was absent. */
uint32_t g_total_mb, g_free_mb = 0xFFFFFFFFu;
