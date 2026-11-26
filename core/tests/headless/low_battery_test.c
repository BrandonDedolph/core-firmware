/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/low_battery_test.c — what the firmware does as the cell runs
 * down, end to end.
 *
 * The real app/battery_ui.c battery_refresh() and the real hal/hw/battery.c
 * policy on the headless PMU ADC (hl_set_battery_mv), on the headless disk:
 *
 *   1. ARMED: nothing fires until the median filter is full, and a healthy
 *      cell writes nothing it should not.
 *   2. DISKSAFE (filtered <= 3500 mV): the edge is narrated, the pending
 *      settings change and the event log's FINAL block go out as the last
 *      writes (CFG_COMMIT_LAST, exempt from the gate — the parked drive is
 *      woken for them), THEN the drive is parked, and the "Battery very low"
 *      modal goes up.
 *   3. Below the line: the modal dismisses on a button; a normal settings
 *      change is HELD — "cfg save deferred — battery below disk-safe", once —
 *      and nothing reaches the disk even with the drive spinning.
 *   4. HYSTERESIS: back up to 3574 mV (above 3500, below the 3600 recover
 *      line) clears nothing; 3644 mV fires RECOVERED, and the held change
 *      lands on the next write opportunity.
 *   5. A second descent, this time while browsing with the drive up and the
 *      input fresh (so the idle spin-down would NOT park it): DISKSAFE parks
 *      it at the edge, the modal comes back (the recovery re-armed the
 *      latch), then 3300 mV: after the 3-evaluation confirm,
 *      SHUTOFF — narrated, the "Battery empty" goodbye lit and held ~1.5 s,
 *      then enter_standby(): its forced settings / evlog commits refused by
 *      the gate (nothing is written below disk-safe), PMU standby, off.
 *
 * Usage: low_battery_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "low-battery", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* ---- helpers (as walk_test.c's) ------------------------------------------ */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int on_screen(const char *s) { return hl_find_text(hl_panel(), s, NULL); }
static int selected(const char *s)
{
    hl_text_hit_t h;
    return hl_find_text_in(hl_panel(), s, 40, HL_H, &h) && lum(h.ink) > lum(h.bg);
}
static int select_row(const char *s)
{
    if (selected(s)) return 1;
    hl_scroll(-16);
    hl_run_ms(16 * 150 + 400);
    for (int i = 0; i < 24; i++) {
        if (selected(s)) return 1;
        hl_scroll(1);
        hl_run_ms(450);
    }
    return selected(s);
}
static void tap(unsigned btn) { hl_tap(btn, 500); }

static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
}
/* Offset of the first `s` at or after `from`, or -1. */
static long log_at(size_t from, const char *s)
{
    if (from > hl_log_len()) return -1;
    const char *p = strstr(hl_log() + from, s);
    return p ? (long)(p - hl_log()) : -1;
}
static int log_count_since(size_t from, const char *s)
{
    int n = 0;
    for (long at = log_at(from, s); at >= 0; at = log_at((size_t)at + 1, s)) n++;
    return n;
}
typedef struct { size_t from; const char *needle; } log_wait_t;
static int pred_log(void *a)
{
    const log_wait_t *w = a;
    return log_has_since(w->from, w->needle);
}
/* Run until `needle` is printed (at or after `from`), in `step_ms` slices. */
static int run_until_log(size_t from, const char *needle, uint32_t step_ms,
                         uint32_t max_ms)
{
    log_wait_t w = { from, needle };
    return hl_run_until(pred_log, &w, step_ms, max_ms);
}

static long ev_find(size_t from, int kind)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    for (size_t i = from; i < n; i++) {
        if (e[i].kind == kind) return (long)i;
    }
    return -1;
}
static long ev_last(size_t from, int kind)
{
    long last = -1;
    for (long k = ev_find(from, kind); k >= 0; k = ev_find((size_t)k + 1, kind)) {
        last = k;
    }
    return last;
}
static uint64_t ev_t(long i)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    return (i >= 0 && (size_t)i < n) ? e[i].t_us : 0;
}

static int copy_disk(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_TRUNC);
    if (in < 0 || out < 0) return -1;
    static char buf[1 << 16];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) return -1;
    }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}
static int load_ranges(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned lba, cnt;
    char name[512];
    int n = 0;
    while (fscanf(f, "%u %u %511[^\n]", &lba, &cnt, name) == 3) {
        if (hl_allow_writes(lba, cnt) == 0) n++;
    }
    fclose(f);
    return n;
}

/* Settings > Playback > Shuffle, SELECT: one real change to g_settings
 * (settings_touch), from the main menu and back to it. Reads no disk, so a
 * parked drive stays parked and the change stays PENDING. */
static int toggle_shuffle(void)
{
    int ok = select_row("Settings");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Playback");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Shuffle");
    tap(HL_BTN_SELECT);
    tap(HL_BTN_MENU);
    tap(HL_BTN_MENU);
    return ok && on_screen("Music");
}

/* Music > Albums > "Long Player": reads its folder, so the drive is up. */
static int spin_drive_up(void)
{
    int ok = select_row("Music");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Albums");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Long Player");
    hl_tap(HL_BTN_SELECT, 800);
    return ok && !hl_stats().ata_parked;
}
static void back_to_menu(void)
{
    for (int i = 0; i < 4; i++) tap(HL_BTN_MENU);
}

#define MODAL_HEAD "Battery very low"
#define MODAL_BODY "Settings will not be saved until then."
#define BYE_HEAD   "Battery empty"
#define BYE_BODY   "Powering off now"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/low_battery_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);
    CHECK("image: the pre-allocated files' extents are known",
          load_ranges(argv[2]) >= 3);

    /* ---- 1. a healthy cell, filter filling ------------------------------ */
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        unlink(disk);
        return 2;
    }
    hl_run_ms(20000);                       /* > 5 samples: the filter is full */
    CHECK("boot: running", hl_parked() == HL_RUNNING);
    CHECK("boot: the battery line reports level 0 on a 3950 mV cell",
          log_has_since(0, " filt 3949 lvl 0 "));
    CHECK("boot: no battery edge on a healthy cell",
          !log_has_since(0, "core: batt DISKSAFE") &&
          !log_has_since(0, "core: batt SHUTOFF"));
    for (int i = 0; i < 30 && !hl_stats().ata_parked; i++) hl_run_ms(1000);
    CHECK("boot: the drive parks while idle", hl_stats().ata_parked);

    size_t log0 = hl_log_len();
    CHECK("a settings change (Shuffle)", toggle_shuffle());
    hl_run_ms(4000);                        /* past the 3 s save debounce */
    CHECK("drive parked: the change is held pending, not written",
          !log_has_since(log0, "core: cfg save rc") && hl_stats().ata_parked);

    /* ---- 2. DISKSAFE ---------------------------------------------------- */
    size_t ev1  = hl_events_mark();
    size_t log1 = hl_log_len();
    hl_set_battery_mv(3450);
    /* Two samples at 3445 in a ring of five leave the median at 3949; the
     * third tips it. Nothing may fire before that. */
    CHECK("3450 mV: DISKSAFE fires",
          run_until_log(log1, "core: batt DISKSAFE: flushing settings, parking drive\n",
                        250, 30000));
    long at_ds = log_at(log1, "core: batt DISKSAFE");
    CHECK("DISKSAFE: not before the median crosses (two low samples first)",
          log_count_since(log1, " mv 3445 clamped 3445 filt 3949 lvl 0 ") == 2);
    CHECK("DISKSAFE: the sample that fired it reads filt 3445 lvl 1",
          log_has_since(log1, " filt 3445 lvl 1 "));
    hl_run_ms(3000);
    long at_cfg = log_at(log1, "core: cfg save rc 00000000");
    long at_log = log_at(log1, " final\n");
    CHECK("DISKSAFE: the pending settings change is the last write (cfg save rc 0)",
          at_ds >= 0 && at_cfg > at_ds);
    CHECK("DISKSAFE: the event log's FINAL block is flushed after it",
          at_cfg >= 0 && at_log > at_cfg && log_has_since(log1, "core: evlog blk "));
    long w_first = ev_find(ev1, HL_EV_ATA_WRITE);
    long w_last  = ev_last(ev1, HL_EV_ATA_WRITE);
    long park    = ev_last(ev1, HL_EV_ATA_STANDBY);
    CHECK("DISKSAFE: the parked drive is woken for the flush",
          ev_find(ev1, HL_EV_ATA_SPINUP) >= 0 &&
          ev_find(ev1, HL_EV_ATA_SPINUP) < w_first);
    CHECK("DISKSAFE: the writes reach the disk", w_first >= 0);
    CHECK("DISKSAFE: THEN the drive is parked", park > w_last && w_last >= 0);
    CHECK("DISKSAFE: parked now", hl_stats().ata_parked);
    CHECK("DISKSAFE: the modal is up", on_screen(MODAL_HEAD) && on_screen(MODAL_BODY));
    CHECK("DISKSAFE: one edge, not one per sample",
          log_count_since(log1, "core: batt DISKSAFE") == 1);
    CHECK("DISKSAFE: still running", hl_parked() == HL_RUNNING);

    /* ---- 3. below the line: writes held --------------------------------- */
    tap(HL_BTN_MENU);
    CHECK("modal: any button dismisses it", !on_screen(MODAL_HEAD) && on_screen("Music"));
    size_t ev2  = hl_events_mark();
    size_t log2 = hl_log_len();
    CHECK("below disk-safe: a settings change", toggle_shuffle());
    CHECK("below disk-safe: the drive spun up by a folder read", spin_drive_up());
    hl_run_ms(4000);
    CHECK("below disk-safe: the change is refused, narrated once",
          log_count_since(log2, "core: cfg save deferred — battery below disk-safe\n") == 1);
    CHECK("below disk-safe: nothing written, drive up or not",
          !log_has_since(log2, "core: cfg save rc") && ev_find(ev2, HL_EV_ATA_WRITE) < 0);
    CHECK("below disk-safe: the modal stays dismissed", !on_screen(MODAL_HEAD));
    back_to_menu();

    /* ---- 4. hysteresis --------------------------------------------------- */
    size_t log3 = hl_log_len();
    hl_set_battery_mv(3580);
    hl_run_ms(30000);
    CHECK("3574 mV (above DISKSAFE, below RECOVER): still level 1",
          log_has_since(log3, " filt 3574 lvl 1 ") &&
          !log_has_since(log3, "core: batt RECOVERED"));
    tap(HL_BTN_MENU);                       /* input, so the idle sleep waits */
    size_t log4 = hl_log_len();
    hl_set_battery_mv(3650);
    CHECK("3644 mV: RECOVERED fires",
          run_until_log(log4, "core: batt RECOVERED: writes allowed again\n", 250, 30000));
    CHECK("RECOVERED: level 0 again", log_has_since(log4, " filt 3644 lvl 0 "));
    CHECK("RECOVERED: no modal", !on_screen(MODAL_HEAD));
    size_t ev5  = hl_events_mark();
    size_t log5 = hl_log_len();
    CHECK("RECOVERED: the drive spun up by a folder read", spin_drive_up());
    hl_run_ms(4000);
    CHECK("RECOVERED: the held change lands (cfg save rc 0)",
          log_has_since(log5, "core: cfg save rc 00000000") &&
          ev_find(ev5, HL_EV_ATA_WRITE) >= 0);
    back_to_menu();

    /* ---- 5. second descent: DISKSAFE, then SHUTOFF ----------------------- */
    /* This time the edge arrives while the user is browsing: the drive is up
     * and the input is fresh, so the idle spin-down would keep it spinning —
     * the park has to be battery_refresh's own. */
    hl_cfg.standby_accept = 1;
    size_t log6 = hl_log_len();
    hl_set_battery_mv(3450);
    CHECK("second descent: browsing, the drive up", spin_drive_up());
    int ds2 = 0;
    for (int i = 0; i < 120 && !ds2; i++) {
        if (i % 4 == 0) hl_scroll((i / 4) & 1 ? -1 : 1);   /* keep input fresh */
        hl_run_ms(250);
        ds2 = log_has_since(log6, "core: batt DISKSAFE");
    }
    CHECK("second descent: DISKSAFE again", ds2);
    CHECK("second descent: the drive is parked at the edge, input still fresh",
          hl_stats().ata_parked);
    hl_run_ms(500);
    CHECK("second descent: the modal again (the recovery re-armed it)",
          on_screen(MODAL_HEAD));
    tap(HL_BTN_MENU);
    back_to_menu();
    CHECK("second descent: a change left pending below the line", toggle_shuffle());

    size_t ev7  = hl_events_mark();
    size_t log7 = hl_log_len();
    hl_set_battery_mv(3300);
    CHECK("3300 mV: SHUTOFF fires",
          run_until_log(log7, "core: batt SHUTOFF: entering standby\n", 50, 40000));
    long at_off = log_at(log7, "core: batt SHUTOFF");
    uint64_t t_off = hl_stats().now_us;
    /* The 3-evaluation confirm on top of the median: the median falls on the
     * 3rd sample at 3300 (run 1), SHUTOFF on the 5th (run 3). */
    CHECK("SHUTOFF: after the confirm (two lvl-1 samples at filt 3300 first)",
          log_count_since(log7, " filt 3300 lvl 1 ") == 2 &&
          log_has_since(log7, " filt 3300 lvl 2 "));
    hl_run_ms(300);
    hl_stats_t s = hl_stats();
    CHECK("SHUTOFF: the goodbye is on the panel",
          on_screen(BYE_HEAD) && on_screen(BYE_BODY));
    CHECK("SHUTOFF: ...lit", s.backlight == 32 && !s.lcd_slept);
    CHECK("SHUTOFF: ...and held, the device not yet off", hl_parked() == HL_RUNNING &&
          ev_find(ev7, HL_EV_PMU_STANDBY) < 0);
    for (int i = 0; i < 40 && hl_parked() == HL_RUNNING; i++) hl_run_ms(100);
    long pmu = ev_find(ev7, HL_EV_PMU_STANDBY);
    CHECK("SHUTOFF: the PMU is told to power off", pmu >= 0);
    CHECK("SHUTOFF: ...after the ~1.5 s goodbye",
          pmu >= 0 && ev_t(pmu) - t_off >= 1400000u && ev_t(pmu) - t_off <= 2500000u);
    CHECK("SHUTOFF: through the standby path",
          log_at(log7, "core: standby: entering") > at_off);
    CHECK("SHUTOFF: the forced settings commit is refused below disk-safe",
          log_at(log7, "core: cfg save deferred — battery below disk-safe") > at_off &&
          !log_has_since(log7, "core: cfg save rc"));
    CHECK("SHUTOFF: the forced evlog commit is refused too",
          log_at(log7, "core: evlog flush deferred — battery below disk-safe") > at_off);
    CHECK("SHUTOFF: nothing written on the way down",
          ev_find(ev7, HL_EV_ATA_WRITE) < 0);
    CHECK("SHUTOFF: from the main loop, not an idle suspend",
          !log_has_since(log6, "core: suspend: entering"));
    CHECK("SHUTOFF: one edge", log_count_since(log7, "core: batt SHUTOFF") == 1);
    CHECK("SHUTOFF: the device is off", hl_parked() == HL_PARK_POWEROFF);
    s = hl_stats();
    CHECK("off: the drive parked, the backlight out", s.ata_parked && s.backlight == 0);

    /* ---- whole session --------------------------------------------------- */
    CHECK("disk: every write inside a pre-allocated file, physically aligned",
          s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);

    unlink(disk);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}
