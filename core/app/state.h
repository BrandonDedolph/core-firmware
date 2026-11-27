/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/state.h — the app-wide singletons: the boot timings, the volume,
 * the live Settings record, the sleep timer and idle-sleep state, the Hold
 * lock, the jack watch and the software clock. Every app module reads some
 * of these, so they are plain extern data (the g_pal precedent, ui/palette.h)
 * rather than accessors. Defined, with their comments, in state.c; moved
 * verbatim out of kernel/main.c.
 *
 * Firmware-only. hal/headless/headless_hw.c and hal/sim/sim_hal.c each have a
 * file-local g_volume of their own: never include this header there.
 */

#ifndef CORE_APP_STATE_H
#define CORE_APP_STATE_H

#include <stdint.h>

#include "../ui/settings.h"
#include "../ui/sleeptimer.h"
#include "../ui/jackwatch.h"
#include "../ui/settime.h"
#include "../kernel/idlesleep.h"
#include "../kernel/wallclock.h"
#include "util.h"

extern uint32_t     g_boot_t0_us;
extern uint32_t     g_boot_lcd_ms;
extern uint32_t     g_boot_disk_ms;
extern uint32_t     g_boot_resume_ms;
/*
 * ...of which. MS_NA (see ui/screen_settings.c) means "this step did not run"
 * — NOT "it took 0 ms". Conflating those made the screen lie exactly when it
 * mattered: after the FLAC seek fix an instant seek looked identical to a
 * skipped one. Seeded to MS_NA before resume_restore(), overwritten by each
 * step that actually executes.
 */
#define BOOT_MS_NA 0xFFFFFFFFu
extern uint32_t     g_boot_res_dir_ms;
extern uint32_t     g_boot_res_open_ms;
extern uint32_t     g_boot_res_seek_ms;
extern uint32_t     g_boot_total_ms;

extern uint32_t     g_diag_cfg_lba[2], g_diag_log_lba[2];

uint32_t boot_ms_now(void);

extern int         g_volume;
extern ui_window_t g_vol_show;
#define VOL_SHOW_US 1500000u

extern settings_t g_settings;

extern sleeptimer_t g_sleep;
extern idlesleep_t  g_idlesleep;

extern int         g_locked;

extern jackwatch_t g_jack;
extern ui_window_t g_lock_flash;
#define LOCK_FLASH_US 1000000u

extern wallclock_t g_wclock;
extern settime_t   g_settime;
extern uint32_t    g_clock_min;
extern uint32_t    g_clock_tick_us;
extern uint32_t    g_clock_resync_us;
extern int         g_clock_retry;

/* Half an hour between RTC reads. Long enough that the bus is effectively
 * untouched, short enough that the USEC_TIMER's ~71.6-minute wrap cannot be
 * missed even if the 5 s cadence stalls (wallclock.h's feed contract). */
#define RTC_RESYNC_S    1800u
/* ...but a read that got no answer is retried in a minute rather than in half
 * an hour: the bus is shared with the codec and a single collision should not
 * cost the clock its correction for a whole cadence. */
#define RTC_RETRY_S     60u
#define CLOCK_TICK_US   5000000u         /* fold the elapsed µs in every 5 s   */

extern uint32_t g_total_mb, g_free_mb;

#endif /* CORE_APP_STATE_H */
