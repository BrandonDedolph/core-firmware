/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/clockui.c — see clockui.h.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hal.h"
#include "../kernel/datetime.h"
#include "../kernel/wallclock.h"
#include "state.h"
#include "clockui.h"

/* ---------------------------------------------------------------------------
 * The clock, as the UI reads it
 * ------------------------------------------------------------------------- */

/* The device's LOCAL epoch right now, as the user is shown it: the software
 * clock plus the stored (STANDARD) offset, plus the daylight hour when
 * Daylight Saving is On and it is in force (kernel/datetime.h). 1 when a time
 * is known, 0 when it is not. */
int clock_local_epoch(uint32_t *local)
{
    uint32_t utc;
    if (!wallclock_now(&g_wclock, mmio_read32(USEC_TIMER_ADDR), &utc)) {
        return 0;
    }
    int off = g_settings.utc_off_min;
    if (off < DATETIME_OFF_MIN || off > DATETIME_OFF_MAX) {
        off = 0;                          /* a record from somewhere else */
    }
    if (off >= 0) {
        *local = utc + (uint32_t)off * 60u;
    } else {
        uint32_t back = (uint32_t)(-off) * 60u;
        if (back > utc) {
            return 0;
        }
        *local = utc - back;
    }
    *local = clock_dst_display(*local, g_settings.dst_on);
    return 1;
}

/* The local time as a civil date; 0 when no time is known. */
int clock_local_now(datetime_t *out)
{
    uint32_t local;
    return clock_local_epoch(&local) && datetime_from_epoch(local, out);
}

/*
 * The clock as the strip and the main menu's header draw it ("10:42 AM" /
 * "22:42"), into the CALLER's buffer — `buf` must hold DATETIME_TIME_MAX bytes.
 * Writes "" when Time in Title is off or no time is known.
 *
 * The caller owns the buffer because both users of this string appear in one
 * paint: main_menu_render passes the header's clock into menu_render_list,
 * which paints the strip (and formats the clock again) BEFORE ui_header reads
 * it. Off one shared static that is a reader of a buffer someone else has
 * already rewritten — today with identical bytes, which is exactly the kind of
 * bug that waits for the two call sites to diverge.
 */
void clock_title_text(char *buf, int buf_sz)
{
    buf[0] = '\0';
    datetime_t now;
    if (!g_settings.time_in_title || !clock_local_now(&now)) {
        return;
    }
    datetime_fmt_time(buf, buf_sz, &now, g_settings.time_24h);
}

/*
 * Re-anchor the software clock from the chip, and narrate what the chip was
 * doing. Called at boot, after a manual set, after a suspend wake and every
 * RTC_RESYNC_S — never per frame: each call is three I2C transactions on the
 * bus the codec shares.
 *
 * The two interesting answers are DEVICE items to read off the log: a drift
 * bigger than a few seconds over half an hour means one of the two clocks is
 * not what we think it is, and a chip that has not moved at all is the shape
 * an absent PMU produces (hal/hw/rtc.c), which is declared "no time" rather
 * than shown as a frozen plausible clock.
 */
/*
 * `trust_timer` says whether the µs counter's delta since the last tick is
 * meaningful. It is on the 30-minute cadence (the loop has been running) and
 * OFF at a wake: the PLL was parked and the tick ran at 10 Hz, so the delta
 * across a suspend is the one number in this module nobody should add to
 * anything. That is why a wake whose RTC read gets no answer drops the clock
 * instead of carrying it — the alternative is a clock wrong by the length of
 * the sleep until the retry lands.
 */
void clock_resync(const char *why, int trust_timer)
{
    uint32_t epoch = 0;
    int      rc    = hal_rtc_get(&epoch);
    uint32_t now   = mmio_read32(USEC_TIMER_ADDR);
    int32_t  drift = 0;

    /*
     * -1 and 0 are the same answer to the BOOT question ("do we know the
     * time?", hal.h) and different answers to this one. 0 means the chip says
     * it has no time, and a software clock running on top of that is a fiction
     * to be dropped. -1 means the bus did not answer — a transient on the bus
     * the codec shares — and the clock we are already carrying is still the
     * best information in the device. Keeping it costs nothing and losing it
     * costs the user their clock for half an hour.
     */
    if (rc < 0 && g_wclock.valid && trust_timer) {
        wallclock_tick(&g_wclock, now);        /* carry on from the timer */
        g_clock_tick_us   = now;
        g_clock_resync_us = now;
        g_clock_retry     = 1;                 /* ...and ask again shortly */
        uart_puts("core: rtc ");
        uart_puts(why);
        uart_puts(" no answer, keeping the software clock\n");
        return;
    }

    wallclock_resync_t r = wallclock_resync(&g_wclock, rc == 1, epoch, now,
                                            &drift);
    g_clock_resync_us = now;
    g_clock_tick_us   = now;
    /* A bus that did not answer is asked again in a minute whatever else this
     * call decided — including the wake, where the clock was just dropped
     * because the park's delta could not be carried. Thirty minutes without a
     * time, after a wake, for one I2C collision is not a cadence, it is an
     * outage (STATUS.md, bench step 5). A chip that answered "no time" is not
     * a retry: it will say the same thing in a minute. */
    g_clock_retry     = (rc < 0) ? 1 : 0;

    uart_puts("core: rtc ");
    uart_puts(why);
    uart_puts(" rc ");
    uart_put_hex32((uint32_t)rc);
    uart_puts(" epoch ");
    uart_put_hex32(epoch);
    if (r == WALLCLOCK_RESYNC_DRIFT) {
        /* On the cadence a drift means one of the two clocks is not what we
         * think it is. At a WAKE it means the obvious thing instead: the µs
         * timer did not run at 1 MHz through the park, so the difference is
         * roughly the length of the sleep. Both print the same way; the `why`
         * on the line is what tells them apart. */
        uart_puts(" drift ");
        uart_put_hex32((uint32_t)drift);
    } else if (r == WALLCLOCK_RESYNC_STOPPED) {
        uart_puts(" stopped");
    }
    uart_putc('\n');
}
