/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/suspend_test.c — suspend_to_ram() and enter_standby() off the
 * idle path.
 *
 * firmware-walk reaches the suspend only through the 120 s idle sleep, by
 * which time the device is paused, dark, parked and has nothing unsaved, so
 * most of what suspend_to_ram() does on the way in is already done for it,
 * and its wake is judged 3 s later, after the main loop has repainted. This
 * drives the other entries — a PLAY hold under a playing track with the
 * screen lit and changes pending, the idle loop's own wake and battery and
 * jack handling, the escalations to a PMU standby — on the headless HAL,
 * which REFUSES standby by default (hl_cfg.standby_accept 0), so the refused-
 * standby recovery runs every time. Each session is its own process.
 *
 * Session 1, PLAY hold under a playing track (spin-up 2.5 s):
 *   in      Long One plays with the drive parked between refill bursts, an
 *           album sits unsaved in On-The-Go and Repeat was just changed (both
 *           pending: nothing writes to a parked drive); PLAY held 6 s. The
 *           forced commits (cfg, otg, evlog FINAL) go out, the drive is
 *           parked again before the white frame, the player is paused, the
 *           boost dropped, the backlight off — and although PLAY was held
 *           past 5 s, the screen had been dark for less than 2.5 s of it, so
 *           it is a sleep, not a power-off.
 *   asleep  the codec powers down (player_pump), the battery is sampled
 *           every 5 s; the disk is copied here (a power cut while asleep).
 *   wake    MENU held 1.5 s (past the 1 s jump-home hold): swallowed, the
 *           Settings page is still up; the FIRST full present after the
 *           wake is that page, not the white frame, and precedes the
 *           backlight; drive woken, clock re-read from the RTC, the track
 *           plays on from where it stopped.
 * Session 2, the copy taken while asleep: Repeat, the On-The-Go list and
 *   the resume position are the ones session 1 had at the suspend.
 * Session 3, the idle loop:
 *   a. headphones pulled and pushed back in while asleep, then a press too
 *      short for the live state (down and up inside one tick): only the
 *      drained events see it, and it wakes; the track stays paused and the
 *      wake says why.
 *   b. the plug pulled at the instant of the waking press (no feed saw it):
 *      no resume into an empty jack, and the watcher is primed, so the main
 *      loop does not report a phantom pull afterwards.
 *   c. on external power the 30-minute escalation does not fire; unplugged
 *      past the deadline it does at once, the PMU refuses, and the device
 *      wakes itself (standby refused), asking once.
 *   d. the cell falls to 3300 mV while asleep: the suspended battery sample
 *      sees DISKSAFE then SHUTOFF, the goodbye is presented, then the white
 *      frame, the PMU refuses, and the suspend wakes itself.
 * Session 4, PLAY held 12 s under a playing track: at ~5 s (and > 2.5 s
 *   dark) enter_standby(): player stopped, audio closed, evlog written and
 *   the drive parked again, the white frame, ONE PMU request. Refused: while
 *   PLAY is still down the panel is repainted and presented and only then
 *   lit; on release the device comes back without another press, idle
 *   sleep off for the session (g_standby_refused seen by run_ui).
 *
 * Throughout: no write outside the pre-allocated files, no panic.
 *
 * Usage: suspend_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "suspend", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

#define BRIGHT 32               /* default g_settings.backlight_bright */

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

static int log_has(const char *s) { return strstr(hl_log(), s) != NULL; }
static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
}
static long log_at(size_t from, const char *s)
{
    if (from > hl_log_len()) return -1;
    const char *p = strstr(hl_log() + from, s);
    return p ? (long)(p - hl_log()) : -1;
}
/* `s` starts in [from, to) of the log. */
static int log_has_between(size_t from, long to, const char *s)
{
    long at = log_at(from, s);
    return at >= 0 && to >= 0 && at < to;
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
static int run_until_log(size_t from, const char *needle, uint32_t step_ms,
                         uint32_t max_ms)
{
    log_wait_t w = { from, needle };
    return hl_run_until(pred_log, &w, step_ms, max_ms);
}

/* First event of `kind` in [from, end), or -1. */
static long ev_find(size_t from, int kind)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    for (size_t i = from; i < n; i++) {
        if (e[i].kind == kind) return (long)i;
    }
    return -1;
}
/* ...with exactly `arg`. */
static long ev_find_arg(size_t from, int kind, uint32_t arg)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    for (size_t i = from; i < n; i++) {
        if (e[i].kind == kind && e[i].arg == arg) return (long)i;
    }
    return -1;
}
/* Last event of `kind` in [from, to), or -1. */
static long ev_last_before(size_t from, long to, int kind)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    long last = -1;
    for (size_t i = from; i < n && (long)i < to; i++) {
        if (e[i].kind == kind) last = (long)i;
    }
    return last;
}
static int ev_count(size_t from, int kind)
{
    size_t n;
    int c = 0;
    const hl_ev_t *e = hl_events(&n);
    for (size_t i = from; i < n; i++) c += e[i].kind == kind;
    return c;
}
static uint32_t ev_arg(long i)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    return i >= 0 && (size_t)i < n ? e[i].arg : 0;
}
static uint64_t ev_t(long i)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    return i >= 0 && (size_t)i < n ? e[i].t_us : 0;
}

/* HL_SHOTS=DIR saves a step's panel there (debugging). */
static void shot(const char *name)
{
    const char *dir = getenv("HL_SHOTS");
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/suspend_%s_%s.bmp", dir, T.suite, name);
    (void)hl_write_bmp(path);
}

static int all_white(void)
{
    return hl_count_color(hl_panel(), 0xFFFF, 0, 0, HL_W, HL_H) == HL_W * HL_H;
}

/* A presented frame that is a screen, not the white "off" frame: the Linen
 * surfaces are not white, so a real screen has tens of thousands of non-white
 * pixels (PRESENT_FULL's arg, hal/headless). */
#define REAL_FRAME_INK 20000u

static int copy_disk(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (in < 0 || out < 0) return -1;
    static char buf[1 << 16];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) return -1;
    }
    close(in);
    return close(out);
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

static void boot(const char *disk, const char *ranges)
{
    CHECK("image: the pre-allocated files' extents are known", load_ranges(ranges) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        _exit(2);
    }
    CHECK("boot: reached the browser", log_has("core: entering browser"));
}

static _Noreturn void session_end(const char *what)
{
    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: fake time %.1f s, %u ATA writes\n", what,
           (double)s.now_us / 1e6, s.ata_writes);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);                     /* the firmware thread is frozen mid-wait */
}

static void to_main_menu(void)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
}

/* Main menu -> Music > Albums > `album`, entered (its folder is read). */
static int open_album(const char *album)
{
    to_main_menu();
    int ok = select_row("Music");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Albums");
    tap(HL_BTN_SELECT);
    ok = ok && select_row(album);
    hl_tap(HL_BTN_SELECT, 800);
    return ok;
}

/* Long One playing on Now Playing. */
static int play_long_one(void)
{
    int ok = open_album("Long Player") && select_row("Long One");
    hl_tap(HL_BTN_SELECT, 1500);
    return ok && hl_stats().audio_running && on_screen("Now Playing");
}

/* Main menu -> Settings > Playback, the Repeat row selected. */
static int to_repeat_row(void)
{
    to_main_menu();
    int ok = select_row("Settings");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Playback");
    tap(HL_BTN_SELECT);
    return ok && select_row("Repeat");
}
static int repeat_reads(const char *v)
{
    hl_text_hit_t r;
    if (!hl_find_text_in(hl_panel(), "Repeat", 40, HL_H, &r)) return 0;
    return hl_find_text_in(hl_panel(), v, r.y - 8, r.y + r.h + 8, NULL);
}

/* Now Playing's elapsed clock ("M:SS", left half of the transport band). */
#define BAND_Y 180
#define BAND_H 40
static int np_elapsed_s(void)
{
    char t[16];
    hl_text_hit_t h;
    for (int sec = 0; sec < 600; sec++) {
        snprintf(t, sizeof t, "%d:%02d", sec / 60, sec % 60);
        if (hl_find_text_in(hl_panel(), t, BAND_Y, BAND_Y + BAND_H, &h) &&
            h.x < HL_W / 2) {
            return sec;
        }
    }
    return -1;
}
static int to_now_playing(void)
{
    to_main_menu();
    int ok = select_row("Now Playing");
    tap(HL_BTN_SELECT);
    return ok && np_elapsed_s() >= 0;       /* the transport band is up */
}

/* Hold PLAY for `hold_ms` (past PLAY_HOLD_US = 2 s: a suspend) and run until
 * the suspend reaches its idle loop. */
static int play_hold_suspend(uint32_t hold_ms, size_t from)
{
    hl_press(HL_BTN_PLAY, hold_ms);
    return run_until_log(from, "core: suspend: idle loop", 100, hold_ms + 8000);
}

/* ---- session 1: PLAY hold, playing, lit, changes pending ------------------ */

static int g_report_fd = -1;

static _Noreturn void session_playhold(const char *disk, const char *ranges,
                                       const char *snap)
{
    T.suite = "suspend(1 play-hold)";
    boot(disk, ranges);

    /* Long One plays; lock until the player parks the drive between refill
     * bursts (the anti-skip buffer holds the rest), unlock. */
    CHECK("in: Long One plays", play_long_one());
    hl_set_hold(1);
    int parked = 0;
    for (int i = 0; i < 60 && !parked; i++) {
        hl_run_ms(500);
        parked = hl_stats().ata_parked;
    }
    hl_set_hold(0);
    hl_run_ms(500);
    CHECK("in: the player parked the drive under the track", parked);

    size_t log_pend = hl_log_len();
    size_t ev_pend = hl_events_mark();
    /* An album into On-The-Go (from the index in RAM: no disk read). */
    to_main_menu();
    CHECK("in: Music", select_row("Music"));
    tap(HL_BTN_SELECT);
    CHECK("in: Albums", select_row("Albums"));
    tap(HL_BTN_SELECT);
    CHECK("in: Beethoven Bits", select_row("Beethoven Bits"));
    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(800);
    CHECK("in: holding Select adds the album to On-The-Go", on_screen("Added 3 songs"));
    hl_run_ms(1000);
    /* ...and a setting, left on its page (no settings_leave). */
    CHECK("in: Settings > Playback > Repeat", to_repeat_row());
    CHECK("in: Repeat starts Off", repeat_reads("Off"));
    hl_tap(HL_BTN_SELECT, 400);
    CHECK("in: Repeat now reads All", repeat_reads("All"));
    hl_run_ms(1000);

    hl_stats_t s = hl_stats();
    CHECK("in: both changes are still pending (nothing written)",
          !log_has_since(log_pend, "core: cfg save") &&
          !log_has_since(log_pend, "core: otg save") &&
          ev_find(ev_pend, HL_EV_ATA_WRITE) < 0);
    CHECK("in: playing, drive parked, screen lit, boosted",
          s.audio_running && s.ata_parked && s.backlight == BRIGHT && s.cpu_boost >= 1);

    /* PLAY held 6 s. A parked drive takes 2.5 s to spin up for the forced
     * settings write, so the screen goes dark ~4.5 s into the hold: at the
     * release it has been dark for well under SUSPEND_ESCALATE_DARK_US. */
    hl_cfg.spinup_ms = 2500;
    size_t log0 = hl_log_len();
    size_t ev0 = hl_events_mark();
    uint64_t t_down = hl_stats().now_us;
    CHECK("suspend: a PLAY hold reaches the idle loop", play_hold_suspend(6000, log0));
    hl_run_ms((uint32_t)((t_down + 6500000u - hl_stats().now_us) / 1000u));
    s = hl_stats();
    hl_cfg.spinup_ms = 800;

    long at_in = log_at(log0, "core: suspend: entering");
    CHECK("suspend: entered", at_in >= 0);
    CHECK("suspend: the pending setting is force-written first",
          log_has_between(log0, at_in, "core: cfg save rc 00000000 seq "));
    CHECK("suspend: the pending On-The-Go list is force-written first",
          log_has_between(log0, at_in, "core: otg save rc 00000000 seq "));
    {
        long at_blk = log_at((size_t)at_in, "core: evlog blk ");
        long at_idle = log_at((size_t)at_in, "core: suspend: idle loop");
        CHECK("suspend: the event log's FINAL block goes out before the idle loop",
              at_blk >= 0 && at_idle > at_blk &&
              log_at((size_t)at_blk, " final\n") == log_at((size_t)at_blk, "\n") -
                                                  (long)strlen(" final"));
    }
    long i_dark = ev_find_arg(ev0, HL_EV_BACKLIGHT, 0);
    long i_white = ev_last_before(ev0, i_dark, HL_EV_PRESENT_FULL);
    long i_wr = ev_last_before(ev0, i_dark, HL_EV_ATA_WRITE);
    long i_park = ev_last_before(ev0, i_dark, HL_EV_ATA_STANDBY);
    CHECK("suspend: the backlight goes out", i_dark >= 0 && s.backlight == 0);
    CHECK("suspend: ...over a white frame presented just before",
          i_white >= 0 && ev_arg(i_white) == 0 && all_white());
    CHECK("suspend: the drive the commits spun up is parked again before the dark",
          i_wr >= 0 && i_park > i_wr && i_park < i_white && s.ata_parked);
    CHECK("suspend: the player is paused", !s.audio_running);
    CHECK("suspend: the 80 MHz boost is released", s.cpu_boost == 0);
    uint64_t t_dark = ev_t(i_dark);
    CHECK("suspend: (the hold ran > 5 s, the dark < 2.5 s of it)",
          i_dark >= 0 && t_down + 6000000u - t_dark < 2500000u &&
          t_dark - t_down > 2000000u);
    CHECK("suspend: ...so a sleep, not a power-off",
          !log_has_since(log0, "core: standby: entering") &&
          s.pmu_standby_calls == 0 && ev_find(ev0, HL_EV_PMU_STANDBY) < 0);
    /* Where the track stopped: one 44.1 kHz track since boot, from 0:00. */
    int at_s = (int)(s.audio_frames_played / 44100u);
    CHECK("suspend: (the track had played a while)", at_s >= 10);

    /* ---- asleep ---- */
    size_t log_a = hl_log_len();
    size_t ev_a = hl_events_mark();
    hl_stats_t sa = hl_stats();
    hl_run_ms(20000);
    s = hl_stats();
    CHECK("asleep: the codec is powered down under the suspend (player_pump)",
          ev_find(ev0, HL_EV_AUDIO_SUSPEND) >= 0 && s.audio_cold);
    CHECK("asleep: the battery is sampled every 5 s",
          log_count_since(log_a, "core: batt raw ") >= 3);
    CHECK("asleep: nothing presented, the drive left parked",
          s.presents_full == sa.presents_full && s.ata_spinups == sa.ata_spinups &&
          s.ata_parked && ev_find(ev_a, HL_EV_ATA_WRITE) < 0);
    CHECK("asleep: still asleep", !log_has_since(log0, "core: suspend: wake") &&
          s.backlight == 0);
    CHECK("asleep: the disk is copied (a power cut here)", copy_disk(disk, snap) == 0);
    if (g_report_fd >= 0) {
        (void)!write(g_report_fd, &at_s, sizeof at_s);
        close(g_report_fd);
    }

    /* ---- wake: MENU held past the 1 s jump-home hold ---- */
    size_t log_w = hl_log_len();
    size_t ev_w = hl_events_mark();
    hl_press(HL_BTN_MENU, 1500);
    hl_run_ms(3000);
    s = hl_stats();
    CHECK("wake: a key wakes it", log_has_since(log_w, "core: suspend: wake\n"));
    CHECK("wake: the clock is re-read from the RTC",
          log_has_since(log_w, "core: rtc wake rc "));
    long i_p1 = ev_find(ev_w, HL_EV_PRESENT_FULL);
    long i_lit = ev_find_arg(ev_w, HL_EV_BACKLIGHT, BRIGHT);
    CHECK("wake: the drive is woken", ev_find(ev_w, HL_EV_ATA_WAKEUP) >= 0);
    CHECK("wake: the FIRST full present is the real screen, not the white frame",
          i_p1 >= 0 && ev_arg(i_p1) >= REAL_FRAME_INK);
    CHECK("wake: ...and comes before the backlight", i_lit > i_p1 && i_p1 >= 0);
    CHECK("wake: the backlight is restored", s.backlight == BRIGHT);
    CHECK("wake: the press is swallowed: still on Settings > Playback",
          repeat_reads("All") && !selected("Music"));
    CHECK("wake: the track plays on", s.audio_running);
    CHECK("wake: boosted again", s.cpu_boost >= 1);
    CHECK("wake: Now Playing", to_now_playing());
    {
        int now_s = (int)(hl_stats().audio_frames_played / 44100u);
        int np = np_elapsed_s();
        CHECK("wake: ...from where it stopped (the clock is the DAC's, +-1 s)",
              np >= at_s && np >= now_s - 1 && np <= now_s + 1);
    }
    session_end("play-hold");
}

/* ---- session 2: the disk as a power cut while asleep left it -------------- */

static _Noreturn void session_snapshot(const char *snap, const char *ranges, int at_s)
{
    T.suite = "suspend(2 cut asleep)";
    boot(snap, ranges);
    CHECK("cut: the On-The-Go album came back from COREOTG.DAT",
          log_has("core: otg load 3 writable 00000001 "));
    CHECK("cut: Settings > Playback > Repeat", to_repeat_row());
    CHECK("cut: Repeat reads All", repeat_reads("All"));
    CHECK("cut: Now Playing", to_now_playing());
    shot("np");
    CHECK("cut: Long One, paused", on_screen("Long One") && !hl_stats().audio_running);
    int np = np_elapsed_s();
    CHECK("cut: at the second the suspend stopped it (+-1 s)",
          at_s > 0 && np >= at_s - 1 && np <= at_s + 1);
    session_end("cut");
}

/* ---- session 3: the idle loop ---------------------------------------------- */

static _Noreturn void session_idle(const char *disk, const char *ranges)
{
    T.suite = "suspend(3 idle loop)";
    boot(disk, ranges);
    CHECK("in: Long One plays", play_long_one());

    /* a. pulled and re-seated while asleep; a drained-only press wakes it */
    size_t log0 = hl_log_len();
    CHECK("a: a PLAY hold suspends", play_hold_suspend(2500, log0));
    hl_run_ms(1500);
    hl_set_headphones(0);
    hl_run_ms(1000);
    hl_set_headphones(1);
    hl_run_ms(1000);
    CHECK("a: a pulled plug does not wake it", !log_has_since(log0, "core: suspend: wake"));
    size_t log_w = hl_log_len();
    hl_press(HL_BTN_SELECT, 0);                 /* down and up in one tick */
    hl_run_ms(2000);
    CHECK("a: a press seen only by the drained events wakes it",
          log_has_since(log_w, "core: suspend: wake\n"));
    CHECK("a: the wake says the plug came out while asleep",
          log_has_since(log_w, "core: jack out during suspend, staying paused"));
    CHECK("a: ...and the track stays paused, though the plug is back",
          !hl_stats().audio_running && hl_stats().backlight == BRIGHT);

    /* b. pulled at the instant of the waking press */
    tap(HL_BTN_PLAY);
    CHECK("b: PLAY resumes", hl_stats().audio_running);
    log0 = hl_log_len();
    CHECK("b: a PLAY hold suspends", play_hold_suspend(2500, log0));
    hl_run_ms(1500);
    log_w = hl_log_len();
    /* The press first, latched on the next 10 ms tick; the plug comes out
     * just after it, before the loop's next look at the wheel — which ends
     * the loop ahead of any jack feed. */
    hl_press(HL_BTN_MENU, 300);
    hl_run_ms(15);
    hl_set_headphones(0);
    hl_run_ms(2500);
    CHECK("b: woke", log_has_since(log_w, "core: suspend: wake\n"));
    CHECK("b: no resume into an empty jack", !hl_stats().audio_running);
    CHECK("b: (no loop pass saw the pull)",
          !log_has_since(log_w, "core: jack out during suspend"));
    CHECK("b: the watcher was primed at the wake: no phantom pull after it",
          !log_has_since(log_w, "core: jack out"));
    hl_set_headphones(1);
    hl_run_ms(500);

    /* c. external power: no 30-minute escalation; unplugged past it, at once */
    hl_set_external_power(1);
    hl_run_ms(6000);
    tap(HL_BTN_MENU);                           /* the charging screen */
    log0 = hl_log_len();
    size_t ev0 = hl_events_mark();
    CHECK("c: a PLAY hold suspends on external power", play_hold_suspend(2500, log0));
    for (int m = 0; m < 32; m++) hl_run_ms(60000);
    CHECK("c: 32 minutes asleep on external power: no PMU standby",
          ev_find(ev0, HL_EV_PMU_STANDBY) < 0 &&
          !log_has_since(log0, "core: standby: entering"));
    CHECK("c: ...still asleep", !log_has_since(log0, "core: suspend: wake") &&
          hl_stats().backlight == 0);
    size_t log_u = hl_log_len();
    size_t ev_u = hl_events_mark();
    hl_set_external_power(0);
    hl_run_ms(1000);
    CHECK("c: unplugged past the deadline: standby at once",
          log_has_since(log_u, "core: standby: entering") &&
          ev_find(ev_u, HL_EV_PMU_STANDBY) >= 0);
    hl_run_ms(3000);
    CHECK("c: refused, asked ONCE", ev_count(ev_u, HL_EV_PMU_STANDBY) == 1);
    CHECK("c: the suspend wakes itself (standby refused), no press",
          log_has_since(log_u, "core: suspend: wake (standby refused)"));
    CHECK("c: run_ui sees the refusal: idle sleep off for the session",
          log_has_since(log_u, "core: idle sleep: off for this session (standby refused)"));
    CHECK("c: lit, the screen back", hl_stats().backlight == BRIGHT && !all_white());

    /* d. the cell runs out while asleep */
    log0 = hl_log_len();
    ev0 = hl_events_mark();
    CHECK("d: a PLAY hold suspends", play_hold_suspend(2500, log0));
    size_t log_d = hl_log_len();
    hl_set_battery_mv(3300);
    CHECK("d: the suspended battery sample reaches SHUTOFF",
          run_until_log(log_d, "core: batt SHUTOFF: entering standby", 250, 90000));
    CHECK("d: ...through DISKSAFE, still asleep",
          log_has_between(log_d, log_at(log_d, "core: batt SHUTOFF"),
                          "core: batt DISKSAFE") &&
          !log_has_between(log_d, log_at(log_d, "core: batt SHUTOFF"),
                           "core: suspend: wake"));
    hl_run_ms(4000);
    long i_pmu = ev_find(ev0, HL_EV_PMU_STANDBY);
    long i_white = ev_last_before(ev0, i_pmu, HL_EV_PRESENT_FULL);
    long i_bye = ev_last_before(ev0, i_white, HL_EV_PRESENT_FULL);
    CHECK("d: the PMU is asked once", i_pmu >= 0 && ev_count(ev0, HL_EV_PMU_STANDBY) == 1);
    CHECK("d: the goodbye, then the white frame, then the PMU",
          i_bye >= 0 && ev_arg(i_bye) >= REAL_FRAME_INK && ev_arg(i_white) == 0);
    CHECK("d: refused: the suspend wakes itself, no press",
          log_has_since(log_d, "core: suspend: wake (standby refused)"));
    CHECK("d: lit, the screen back", hl_stats().backlight == BRIGHT && !all_white());
    session_end("idle loop");
}

/* ---- session 4: PLAY held into a (refused) power-off ----------------------- */

static _Noreturn void session_standby(const char *disk, const char *ranges)
{
    T.suite = "suspend(4 standby)";
    boot(disk, ranges);
    CHECK("in: Long One plays", play_long_one());
    hl_run_ms(3000);

    size_t log0 = hl_log_len();
    size_t ev0 = hl_events_mark();
    uint64_t t_down = hl_stats().now_us;
    hl_press(HL_BTN_PLAY, 12000);
    hl_run_ms(9000);                            /* PLAY still down */
    hl_stats_t s = hl_stats();
    long at_sb = log_at(log0, "core: standby: entering");
    long i_pmu = ev_find(ev0, HL_EV_PMU_STANDBY);
    CHECK("hold: suspended first", log_has_since(log0, "core: suspend: entering"));
    CHECK("hold: past 5 s it escalates to enter_standby", at_sb >= 0 && i_pmu >= 0);
    CHECK("hold: ...not before 5 s from the down-edge",
          i_pmu >= 0 && ev_t(i_pmu) - t_down > 5000000u);
    CHECK("standby: the audio is closed", s.audio_open == 0 &&
          ev_find(ev0, HL_EV_AUDIO_CLOSE) >= 0 &&
          ev_find(ev0, HL_EV_AUDIO_CLOSE) < i_pmu);
    {
        long i_wr = ev_last_before(ev0, i_pmu, HL_EV_ATA_WRITE);
        long i_park = ev_last_before(ev0, i_pmu, HL_EV_ATA_STANDBY);
        CHECK("standby: the evlog block is written before the power cut",
              i_wr >= 0 && log_has_since((size_t)at_sb, " final\n"));
        CHECK("standby: ...and the drive parked after it", i_park > i_wr);
    }
    long i_white = ev_last_before(ev0, i_pmu, HL_EV_PRESENT_FULL);
    long i_dark = ev_last_before(ev0, i_pmu, HL_EV_BACKLIGHT);
    CHECK("standby: white frame, backlight off, then the PMU",
          i_white >= 0 && ev_arg(i_white) == 0 && i_dark >= 0 && ev_arg(i_dark) == 0);
    CHECK("standby: the PMU is asked ONCE", s.pmu_standby_calls == 1);
    CHECK("refused: narrated", log_has_since((size_t)at_sb, "core: standby REFUSED by the PMU"));
    long i_rp = ev_find((size_t)i_pmu + 1, HL_EV_PRESENT_FULL);
    long i_rl = ev_find_arg((size_t)i_pmu + 1, HL_EV_BACKLIGHT, BRIGHT);
    CHECK("refused: the screen is repainted and presented",
          i_rp >= 0 && ev_arg(i_rp) >= REAL_FRAME_INK && !all_white());
    CHECK("refused: ...then lit", i_rl > i_rp && s.backlight == BRIGHT);
    CHECK("refused: all while PLAY is still held (not the wake path)",
          !log_has_since(log0, "core: suspend: wake"));

    hl_run_ms(4000);                            /* PLAY released at 12 s */
    s = hl_stats();
    CHECK("refused: on release the device comes back, no further press",
          log_has_since(log0, "core: suspend: wake (standby refused)\n"));
    CHECK("refused: the PMU was still asked only once", s.pmu_standby_calls == 1);
    CHECK("refused: run_ui sees g_standby_refused (idle sleep off)",
          log_has_since(log0, "core: idle sleep: off for this session (standby refused)"));
    CHECK("refused: lit, not resumed", s.backlight == BRIGHT && !s.audio_running);
    shot("refused_woke");
    /* Stopped, not paused: with nothing loaded Now Playing gives way to the
     * tracklist it was opened from. */
    CHECK("refused: the player was STOPPED, not paused (Now Playing gone)",
          selected("Long One") && on_screen("Long Two") && np_elapsed_s() < 0);
    size_t log_i = hl_log_len();
    hl_run_ms(130000);
    CHECK("refused: no idle sleep for the rest of the session",
          !log_has_since(log_i, "core: suspend: entering"));
    session_end("standby");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/suspend_XXXXXX";
    char snap[] = "/tmp/suspend_snap_XXXXXX";
    int fd = mkstemp(disk);
    int fs = mkstemp(snap);
    if (fd < 0 || fs < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);
    close(fs);

    int ok = 1, st;
    int pfd[2];
    if (pipe(pfd) != 0) return 2;
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        close(pfd[0]);
        g_report_fd = pfd[1];
        session_playhold(disk, argv[2], snap);
    }
    close(pfd[1]);
    int at_s = -1;
    if (read(pfd[0], &at_s, sizeof at_s) != (ssize_t)sizeof at_s) at_s = -1;
    close(pfd[0]);
    waitpid(p, &st, 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        fprintf(stderr, "suspend: session 1 (play-hold) FAILED\n");
        ok = 0;
    }

    struct { const char *name; int which; } runs[] = {
        { "2 (cut asleep)", 2 }, { "3 (idle loop)", 3 }, { "4 (standby)", 4 },
    };
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        /* Sessions 3 and 4 start from the pristine image. */
        if (runs[i].which != 2 && copy_disk(argv[1], disk) != 0) return 2;
        fflush(NULL);
        p = fork();
        if (p == 0) {
            if (runs[i].which == 2) session_snapshot(snap, argv[2], at_s);
            if (runs[i].which == 3) session_idle(disk, argv[2]);
            session_standby(disk, argv[2]);
        }
        waitpid(p, &st, 0);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            fprintf(stderr, "suspend: session %s FAILED\n", runs[i].name);
            ok = 0;
        }
    }
    unlink(disk);
    unlink(snap);
    return ok ? 0 : 1;
}
