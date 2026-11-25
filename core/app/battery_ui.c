/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/battery_ui.c — see battery_ui.h.
 */

#include <stdint.h>

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/ata.h"
#include "hw/backlight.h"
#include "hw/battery.h"
#include "hw/power.h"
#include "hw/audio.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../kernel/chargestat.h"
#include "../ui/text.h"
#include "../ui/screen_charging.h"
#include "../ui/screen_battery.h"
#include "../ui/settings.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "state.h"
#include "util.h"
#include "persist.h"
#include "main_internal.h"
#include "battery_ui.h"

/* Cached battery/power readout. The gauge read is an I2C transaction (slow, and
 * shares the codec bus), so sample it a few seconds apart — NOT every present —
 * and hold the last value. mv/pct < 0 means the read failed / not yet sampled. */
/* The charger's input budget (LTC4066 HPWR) is Settings > Battery > Charge
 * Rate now — 500 mA by default, 100 mA as the quiet option (2026-09-13's
 * ground-loop finding). charger_apply() pushes the setting to the pin; the
 * gauge corrects for whatever charger_max_current() says was asked. */

/* What the cell has been doing lately (kernel/chargestat.h): fed by every
 * battery sample, read by the Battery page and the charging screen's
 * caption. */
chargestat_t g_chg;

int      g_bat_mv  = -1;
int      g_bat_pct = -1;
static int      g_bat_ext = 0;               /* external power present            */
static uint32_t g_bat_last_us;
/* Battery sample cadence (battery_refresh / battery_due). */
#define BATTERY_SAMPLE_US  5000000u
int      g_bat_raw = -1;              /* 10-bit ADC code, for calibration  */
static int      g_bat_mv_raw = -1;           /* mV before the plausibility clamp  */
int      g_bat_mv_filt = -1;          /* median of recent samples (policy) */

/*
 * Push Settings > Battery > Charge Rate to the LTC4066. Called at run_ui's
 * start (defaults), from settings_apply() after config_load and after every
 * row change, so the pin always follows the record. Idempotent and cheap
 * (six masked GPIO writes), so re-applying with the codec settings is fine.
 */
void charger_apply(void)
{
    charger_set_max_current(settings_charge_ma(&g_settings));
}

/*
 * The charging screen, plus its caption: the numbers the big percent hides.
 * "3912 mV · 500 mA · +38 mV in 12 min" — the session drift is the one
 * honest answer to "is it actually charging" this device can give (see
 * kernel/chargestat.h), and it belongs on the screen that pops up when the
 * cable goes in, not only on a Settings page.
 */
static void note_cat(char *d, const char *w)
{
    while (*d) d++;
    while (*w) *d++ = *w++;
    *d = '\0';
}

void charging_screen_paint(void)
{
    screen_charging_render(g_bat_pct, power_is_charging(), power_is_external());
    char note[64], n[16];
    note[0] = '\0';
    if (g_bat_mv_filt >= 0) {
        u32_to_dec(n, (unsigned)g_bat_mv_filt);
        note_cat(note, n); note_cat(note, " mV");
    }
    if (power_is_external()) {
        if (note[0]) note_cat(note, " " UI_GLYPH_MIDDOT " ");
        u32_to_dec(n, (unsigned)charger_max_current());
        note_cat(note, n); note_cat(note, " mA");
    }
    uint32_t secs = chargestat_session_s(&g_chg, mmio_read32(USEC_TIMER_ADDR));
    if (chargestat_session_mv0(&g_chg) >= 0 && secs >= 60u) {
        /* Once the session is a minute old; "+0 mV in <1 min" says nothing. */
        int d = chargestat_session_delta_mv(&g_chg);
        if (note[0]) note_cat(note, " " UI_GLYPH_MIDDOT " ");
        if (d > 0) note_cat(note, "+");
        if (d < 0) { note_cat(note, "-"); d = -d; }
        u32_to_dec(n, (unsigned)d);
        note_cat(note, n); note_cat(note, " mV in ");
        u32_to_dec(n, secs / 60u);
        note_cat(note, n); note_cat(note, " min");
    }
    screen_charging_note(note);
    if (g_locked) screen_charging_lock_render();
}

/* The low-battery modal, painted the same way: the dark field has no strip
 * for the Hold padlock to live in and no chrome for the banner to take
 * (banner_hosted), so the corner glyph is all Hold shows on either modal. */
void battery_modal_paint(void)
{
    screen_battery_render(BATTWARN_DISKSAFE);
    if (g_locked) screen_charging_lock_render();
}

/* Would battery_refresh(0) sample now? The suspend loop asks first so it can
 * bring the clocks back before the sample rather than after finding out. */
int battery_due(void)
{
    return (uint32_t)(mmio_read32(USEC_TIMER_ADDR) - g_bat_last_us)
           >= BATTERY_SAMPLE_US;
}

/* Returns 1 if it actually resampled this call (so a live screen can repaint). */
int battery_refresh(int force)
{
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    if (!force && (uint32_t)(now - g_bat_last_us) < BATTERY_SAMPLE_US) {
        return 0;
    }
    g_bat_last_us = now;

    /*
     * ONE conversion. This used to call battery_millivolts() and then
     * battery_percent(), which runs its own — two I2C round trips, two settling
     * delays, and two DIFFERENT samples, so the millivolts on screen did not
     * necessarily correspond to the percentage next to them. It also left
     * nowhere to put a filter later.
     */
    battery_sample_t bs;
    if (battery_sample(&bs) != 0) {
        /* Bus failure is NOT a flat battery. Hold the last good reading rather
         * than reporting -1, which draw_battery() clamps to 0% — i.e. an empty
         * red battery for an I2C hiccup. The policy is told the same thing:
         * battery_policy_feed(-1) touches neither the filter nor the level, so
         * a flaky bus can neither power the device off nor clear a genuine
         * DISKSAFE. */
        (void)battery_policy_feed(-1, power_is_external());
        uart_puts("core: batt read failed\n");
        return 1;
    }
    g_bat_raw    = bs.raw;
    g_bat_mv_raw = bs.mv_raw;
    g_bat_mv     = bs.mv;
    g_bat_ext    = power_is_external();

    /*
     * Low-battery policy (battery.h). The sample goes into the median filter
     * and the policy decides; we act on the EDGE it returns, so each action
     * happens once per crossing. The displayed percentage is converted from
     * the FILTERED millivolts too — same curve, same mapping, just not
     * re-evaluated on a single spin-up-sagged sample every 5 s.
     */
    battery_event_t ev = battery_policy_feed(bs.mv, g_bat_ext);
    g_bat_mv_filt = battery_filtered_mv();
    /*
     * The gauge: charge-corrected while on external power (the terminal
     * voltage is lifted by the charge current; see battery_percent_charging),
     * then SLEWED. A voltage-derived percent steps when the cable goes in or
     * out and when the drive spins up; the user reads "the battery", not the
     * terminal voltage, so the shown number moves at most one point per
     * sample (5 s) toward the estimate. Seeded on the first good sample.
     */
    int est = g_bat_ext ? battery_percent_charging(g_bat_mv_filt,
                                                   charger_max_current())
                        : battery_percent_from_mv(g_bat_mv_filt);
    if (g_bat_pct < 0 || est < 0) {
        g_bat_pct = est;                        /* first sample, or none */
    } else if (est > g_bat_pct) {
        g_bat_pct++;
    } else if (est < g_bat_pct) {
        g_bat_pct--;
    }

    /* Same sample, same cadence, feeds what the USER is told. Everything the
     * warning UI does about flapping keys off the FILTERED millivolts and the
     * policy's own level, never a raw reading — see ui/screen_battery.c. */
    battwarn_feed(battery_filter_ready() ? g_bat_mv_filt : -1,
                  (int)battery_policy_level(), g_bat_ext,
                  mmio_read32(USEC_TIMER_ADDR));

    /*
     * One line per sample, so a full discharge can be logged over UART and
     * turned into numbers. There is otherwise NO way to see either half of the
     * battery story on this device: the percent curve is transcribed from a
     * 2005 cell and has never been checked against the one actually fitted,
     * and per-state drain (idle / paused / playing) has never been measured at
     * all. The state flags are on the line because the drain question is
     * entirely "what was the device doing while it fell".
     *
     * raw is the ground truth (no scaling assumed); mv_raw vs mv shows when the
     * plausibility clamp is engaging, which is the difference between a flat
     * cell and a bad read.
     */
    /* The trend record. Fed the FILTERED value, like the policy: the page
     * draws the cell, not the spin-up sag. A plug/unplug edge is narrated
     * once so the log shows where a session began. */
    int chg_now = power_is_charging();
    if (chargestat_feed(&g_chg, g_bat_mv_filt, g_bat_ext, chg_now, now)) {
        uart_puts(g_bat_ext ? "core: charger in, " : "core: charger out, ");
        uart_puts("session from ");
        uart_dec(g_bat_mv_filt);
        uart_puts(" mV\n");
    }

    uart_puts("core: batt raw ");   uart_dec(g_bat_raw);
    uart_puts(" mv ");              uart_dec(g_bat_mv_raw);
    uart_puts(" clamped ");         uart_dec(g_bat_mv);
    uart_puts(" filt ");            uart_dec(g_bat_mv_filt);
    uart_puts(" lvl ");             uart_dec((int)battery_policy_level());
    uart_puts(" pct ");             uart_dec(g_bat_pct);
    uart_puts(" ext ");             uart_dec(g_bat_ext);
    uart_puts(" chg ");             uart_dec(chg_now);
    /* The budget asked of the charger, and the session's drift: the two
     * numbers the 2026-09-19 log could only be read for by hand. */
    uart_puts(" ma ");              uart_dec(charger_max_current());
    uart_puts(" dmv ");             uart_dec(chargestat_session_delta_mv(&g_chg));
    uart_puts(" play ");            uart_dec(player_active());
    uart_puts(" paused ");          uart_dec(player_paused());
    /* No backlight state: it is a local in the main loop, not a global, and
     * hoisting it just for a log line is not worth it — hold the backlight
     * fixed for the duration of a measurement run instead. */
    uart_puts(" parked ");          uart_dec(ata_is_parked());
    uart_puts(" up ");              uart_dec((int)(now / 1000000u));
    uart_putc('\n');

    /*
     * Audio health, on the same 5 s cadence.
     *
     * `late` is the counter that did not exist until the LCD IRQ-masking work:
     * completions serviced past the ~363 us the I2S FIFO can cover. It is the
     * ONLY machine-readable signal for the tick-while-the-screen-is-busy
     * failure — audio_underruns() cannot see it, because a late ISR produces
     * no short read, and until now the only detector was a person listening.
     * `present` is the measured cost of the last full present, which every
     * throughput argument about the UI has been reasoning against blind: it
     * starts life as a GUESS of 30000 us and has never been printed.
     */
    uart_puts("core: audio late ");  uart_dec((int)audio_late_kicks());
    uart_puts(" worst_us ");         uart_dec((int)audio_late_worst_us());
    uart_puts(" underruns ");        uart_dec((int)audio_underruns());
    uart_puts(" present_us ");       uart_dec((int)g_present_cost_us);
    /*
     * The two readings added for the crunchiness hunt (2026-09-24), each
     * answering one question the three above cannot:
     *   fifo_empty / fifo_worst_free — the tick's 100 Hz sample of the I2S
     *   TX FIFO while the DMA streams (audio.h audio_fifo_service). Climbing
     *   with `late` and `underruns` flat is starvation INSIDE a transfer —
     *   the DMA losing the bus — which no completion-side counter can see.
     *   rg_q8 / capped — the ReplayGain the decoding track runs at, 1/256 dB,
     *   and whether its peak tag capped it. A texture heard only on
     *   `capped 1` tracks is the clipping class this line was added to
     *   settle; heard on `rg_q8 0` tracks too, it is not the gain.
     */
    uart_puts(" fifo_empty ");       uart_dec((int)audio_fifo_empty_samples());
    uart_puts(" fifo_worst_free ");  uart_dec((int)audio_fifo_worst_free());
    {
        const player_stats_t *ps = player_stats();
        uart_puts(" rg_q8 ");        uart_dec(ps ? ps->rg_q8 : 0);
        uart_puts(" capped ");       uart_dec(ps ? ps->rg_capped : 0);
    }
    uart_putc('\n');

    /*
     * Act on the policy's edge. Logged BEFORE acting, because the shutoff
     * branch never returns and the UART line is the only record of why the
     * device went dark.
     */
    switch (ev) {
    case BATTERY_EVENT_DISKSAFE:
        uart_puts("core: batt DISKSAFE: flushing settings, parking drive\n");
        /*
         * The LAST write. Persist the resume position + any pending change
         * NOW, while the filtered cell is still at ~3500 mV and has the
         * energy to finish one sector plus the FLUSH. From here down nothing
         * else writes: the policy latched DISKSAFE before handing us this
         * edge, so battery_disk_writes_allowed() is ALREADY 0 — which is why
         * this one commit is CFG_COMMIT_LAST (exempt from the gate) and every
         * other one, enter_standby()'s included, is refused until RECOVERED.
         * This mirrors suspend_to_ram()'s "last chance to persist": after
         * this the drive is parked and the next event is power-off.
         */
        resume_capture();
        settings_commit(CFG_COMMIT_LAST);
        otg_commit(CFG_COMMIT_LAST);      /* the live list, same exemption */
        /* The log's last write, exempt from the gate the same way: the
         * DISKSAFE line above and everything before it reach the disk;
         * the SHUTOFF that follows is narrated but, by design, never
         * flushed — nothing writes below this line. */
        evlog_commit(CFG_COMMIT_LAST);
        /* Park, unless the player is mid-stream: it parks between its own
         * refill bursts (player.c) and its next read would only spin the
         * platters straight back up. Same guard as the main loop's idle
         * spin-down. Reads are harmless to data; writes are what matters. */
        if (!ata_is_parked() && (!player_active() || player_paused())) {
            ata_standby();
        }
        break;

    case BATTERY_EVENT_SHUTOFF:
        uart_puts("core: batt SHUTOFF: entering standby\n");
        /*
         * Say goodbye before the lights go out.
         *
         * Without this the panel went straight from whatever was on it to
         * black — from the outside indistinguishable from a crash, which is
         * the worst thing a device can look like when the real answer is
         * "plug me in". Pause first so the message is read in quiet rather
         * than over a note cut mid-way, and light the backlight: a device
         * that dies face-up on a desk should be legible.
         *
         * The hold is a USEC_TIMER spin, not cpu_wait_ms(): that takes a
         * uint8_t of milliseconds and may wake early on an IRQ, and this is
         * the one message the user gets.
         *
         * Order: panel awake, frame presented, THEN backlight. This can fire
         * from inside a suspend, where the panel is asleep (LCD_SLEEP) and
         * a present is a no-op until lcd_wake(); and light behind a panel
         * that is still running its wake-init is the white flash. lcd_wake
         * is a no-op when the panel is already up, and the present is then
         * the ordinary one — so the order costs nothing on the main loop.
         */
        player_pause();
        lcd_wake();
        screen_battery_render(BATTWARN_SHUTOFF);
        lcd_present_fb(console_framebuffer());
        backlight_set(g_settings.backlight_bright);
        {
            uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
            while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < 1500000u) {
            }
        }
        /* The documented power-off path: stop the player, blank the panel,
         * PMU deep-sleep with wake sources set. Its own settings_commit(1)
         * is the reason the DISKSAFE flush above exists: by now the write
         * gate should refuse it (see battery_disk_writes_allowed). It comes
         * back only if the PMU refused, already repainted; nothing more to do
         * here — the policy stays latched at SHUTOFF, so this edge does not
         * repeat, and the device runs on until the cell gives out. */
        (void)enter_standby();
        break;

    case BATTERY_EVENT_RECOVERED:
        uart_puts("core: batt RECOVERED: writes allowed again\n");
        /* Nothing else to do: a change left pending by the gate rides out on
         * the main loop's next settings_commit(0). */
        break;

    case BATTERY_EVENT_NONE:
        break;
    }
    return 1;
}

/* Battery glyph: outline + nub + a fill proportional to `pct`. The fill turns a
 * clear RED at <=20% (a low-battery warning), else ink. ~30% larger than the
 * original 17x9 for legibility. Drawn at top-left (x,y), ~24px wide incl. nub. */
void draw_battery(int x, int y, int pct)
{
    const int w = 22, h = 12;
    console_fill_rect(x, y, w, 1, LINEN_MUTED2);           /* top    */
    console_fill_rect(x, y + h - 1, w, 1, LINEN_MUTED2);   /* bottom */
    console_fill_rect(x, y, 1, h, LINEN_MUTED2);           /* left   */
    console_fill_rect(x + w - 1, y, 1, h, LINEN_MUTED2);   /* right  */
    console_fill_rect(x + w, y + 4, 2, h - 8, LINEN_MUTED2); /* nub   */
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int fw = ((w - 4) * pct) / 100;
    if (fw > 0) {
        console_fill_rect(x + 2, y + 2, fw, h - 4,
                          pct <= 20 ? BATT_LOW_RED : LINEN_INK);
    }
}

/* What draw_battery() would actually put on the panel for `pct`: the fill
 * width in pixels and whether it is red. Two samples that round to the same
 * picture are the same picture, and must not cost a repaint. */
int battery_glyph_key(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return ((18 * pct) / 100) | ((pct <= 20) ? 0x100 : 0);
}
