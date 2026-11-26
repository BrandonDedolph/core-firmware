/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/np_settings_input_test.c — the Now Playing wheel (volume and
 * the scrubber) and the Settings sliders and Date & Time editor, driven end to
 * end through run_ui's input switch.
 *
 * The other headless tests reach Now Playing and Settings but never turn the
 * wheel there: nothing checks that the codec follows the volume wheel, that
 * the volume plate repaints while it is up, that the scrubber aims and seeks,
 * that a slider change is saved, or that the Date & Time editor writes the
 * clock it shows (or writes nothing on MENU). ui/settime.c's unit test covers
 * the editor's MODEL; this covers the wiring — which field the wheel lands
 * on, what SELECT and MENU do, what reaches the RTC and the screen. One
 * power-on of the stock headless disk, nothing playing until part 4:
 *
 *   1. Sound   Volume slider +3: g_settings, g_volume and the codec follow,
 *              and the record is marked for saving (the drive is parked with
 *              nothing playing, so the write itself waits for the next
 *              spin-up or forced commit — kernel/cfg_commit.h). Volume Limit
 *              to 75.
 *   2. Display Backlight cycled to Never (so the clock is still on the panel
 *              a minute later); Brightness slider -4 drives the backlight.
 *   3. Date & Time editor
 *              cancel: the editor opens on the year, seeded from the clock;
 *              the year CLAMPS at 2001 and 2099, the month, day, minute (and
 *              the 12-hour hour inside its half-day) WRAP, the AM/PM plate
 *              flips on an odd detent count, the Settings cursor never moves;
 *              MENU leaves for Date & Time (not the Settings root), no RTC
 *              write, the clock unchanged.
 *              commit: Time in Title on (so the status strip shows the clock),
 *              minute -1 and SELECT through the last field: "core: rtc set"
 *              rc 0, back on Date & Time row 0, the clock reads the time
 *              entered and both the strip and row 0 show it — and the
 *              running time a minute later. Row 0 is the clock
 *              settings_render_cur() injects (settings_set_now); it read
 *              "Not set" on the host while that was one call whose argument
 *              order left `local` unfilled.
 *   4. Now Playing (Long Two, 90 s, a small file the anti-skip buffer holds)
 *              volume: a detent is +1 on g_volume, the Settings record and
 *              the codec; a second detent while the plate is up re-presents
 *              the plate; the Volume Limit pins it at 75; the floor at 0.
 *              scrub: a SELECT tap enters it aimed at the elapsed time; a
 *              detent aims 5 s on (volume untouched), the quiet commit seeks
 *              there; it clamps at 0 and at the track's length; it lapses
 *              after 4 s quiet and the wheel is volume again; a second tap
 *              leaves it. MENU goes back, playing.
 *
 * Throughout: no write outside the pre-allocated files, no panic.
 *
 * Usage: np_settings_input_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "player/player.h"          /* read-only, while the firmware is frozen */
#include "app/nav.h"                /* scr_cur, g_scr, g_scr_n               */
#include "app/lists.h"              /* g_main_sel, g_music_sel, MM_*, MU_*   */
#include "app/library.h"            /* g_songs, g_songview                   */
#include "app/state.h"              /* g_settings, g_volume, g_settime       */
#include "app/settings_glue.h"      /* g_set_screen, g_set_sel, ...          */
#include "app/nowplaying.h"         /* np_scrubbing, g_scrub_target_s        */
#include "app/clockui.h"            /* clock_local_now                       */
#include "app/persist.h"             /* g_cfg_commit                          */
#include "ui/settings.h"            /* SETTINGS_*                            */
#include "ui/settime.h"             /* ST_*                                  */
#include "ui/chrome.h"              /* VOL_PLATE_*                           */
#include "kernel/datetime.h"
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "np-settings-input", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* ---- helpers --------------------------------------------------------------- */

/* `rows` detents, at rest (one per 150 ms: velocity 1), then settle. */
static void wheel(int rows)
{
    hl_scroll(rows);
    hl_run_ms((uint32_t)(rows < 0 ? -rows : rows) * 150u + 400u);
}
/* One event of 12 detents' worth of ticks: wheel_value_step's cap. */
static void flick(int dir, uint32_t settle)
{
    hl_wheel_clicks(dir * 48);
    hl_run_ms(settle);
}
static void tap(unsigned btn, uint32_t settle) { hl_tap(btn, settle); }

static size_t log_mark(void) { return hl_log_len(); }
static int log_has_since(size_t from, const char *s)
{
    return hl_log_len() >= from && strstr(hl_log() + from, s) != NULL;
}

static int on_panel(const char *s) { return hl_find_text(hl_panel(), s, NULL); }

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
static int find_song(const char *title)
{
    for (int i = 0; i < g_songview_n; i++) {
        if (strcmp(g_songs[g_songview[i]].title, title) == 0) return i;
    }
    return -1;
}

/* Rect presents of exactly the volume plate. */
static int g_plate_presents;
static void on_rect(void *user, int x, int y, int w, int h)
{
    (void)user;
    if (x == VOL_PLATE_X && y == VOL_PLATE_Y && w == VOL_PLATE_W && h == VOL_PLATE_H) {
        g_plate_presents++;
    }
}

static int in_settings(int screen)
{
    return scr_cur() == SCR_SETTINGS && g_set_screen == screen;
}
/* The Settings cursor from wherever it is to `row`. */
static void set_row(int row) { if (row != g_set_sel) wheel(row - g_set_sel); }

/* The local clock as the firmware reads it, and as row 0 of Date & Time
 * draws it. */
static int now_local(datetime_t *d)
{
    hl_hold_time(1);
    int ok = clock_local_now(d);
    hl_hold_time(0);
    return ok;
}
/* player_elapsed_s() reads the timer: hold it, as for any firmware call
 * made from the test thread. */
static uint32_t elapsed_s(void)
{
    hl_hold_time(1);
    uint32_t e = player_elapsed_s();
    hl_hold_time(0);
    return e;
}
/* The status strip (the top band) shows `s`. */
static int strip_shows(const char *s) { return hl_find_text_in(hl_panel(), s, 0, STATUS_H + 4, NULL); }
/* Below the strip and the header: the rows. Date & Time's row 0 is the only
 * row that draws a time, so this is row 0 showing `s`. */
static int rows_show(const char *s) { return hl_find_text_in(hl_panel(), s, 40, HL_H, NULL); }
static void fmt_time(char *buf, const datetime_t *d)
{
    datetime_fmt_time(buf, 16, d, g_settings.time_24h);
}

/* ---- 1. Sound ------------------------------------------------------------- */

static void sound(void)
{
    set_row(1);                                     /* Sound */
    tap(HL_BTN_SELECT, 500);
    CHECK("sound: SELECT on Sound opens it, cursor on Volume",
          in_settings(SETTINGS_SOUND) && g_set_sel == 0 && !g_set_editing);
    tap(HL_BTN_SELECT, 500);
    CHECK("sound: SELECT on the Volume slider edits it", g_set_editing == 1);

    /* Nothing has changed the record yet; change ONLY the slider. The drive
     * is parked (nothing playing), so the save itself waits for the next
     * spin-up or forced commit (kernel/cfg_commit.h): what the slider owes
     * is the pending change. */
    int v0 = g_settings.volume;
    CHECK("sound: (no change pending yet)", !g_cfg_commit.dirty);
    wheel(3);
    CHECK("sound: the slider wheel moves the volume by 3 (record, g_volume, codec)",
          g_settings.volume == v0 + 3 && g_volume == v0 + 3 &&
          hl_stats().volume == v0 + 3);
    CHECK("sound: ...and marks the record for saving", g_cfg_commit.dirty);
    tap(HL_BTN_MENU, 500);
    CHECK("sound: MENU ends the edit, stays on Sound",
          in_settings(SETTINGS_SOUND) && !g_set_editing);

    wheel(1);                                       /* Volume Limit */
    tap(HL_BTN_SELECT, 500);
    CHECK("sound: Volume Limit edits", g_set_sel == 1 && g_set_editing == 1);
    int lim0 = g_settings.volume_limit;
    wheel(75 - lim0);
    CHECK("sound: Volume Limit to 75", g_settings.volume_limit == 75);
    tap(HL_BTN_MENU, 500);
    tap(HL_BTN_MENU, 500);
    CHECK("sound: MENU twice: the root, cursor on Sound",
          in_settings(SETTINGS_ROOT) && g_set_sel == 1);
}

/* ---- 2. Display ----------------------------------------------------------- */

static void display(void)
{
    set_row(3);                                     /* Display */
    tap(HL_BTN_SELECT, 500);
    CHECK("display: SELECT opens Display", in_settings(SETTINGS_DISPLAY) && g_set_sel == 0);
    for (int i = 0; i < 6 && g_settings.backlight_secs != 0; i++) {
        tap(HL_BTN_SELECT, 400);
    }
    CHECK("display: Backlight cycled to Never", g_settings.backlight_secs == 0);

    wheel(1);
    tap(HL_BTN_SELECT, 500);
    CHECK("display: the Brightness slider edits", g_set_sel == 1 && g_set_editing == 1);
    int b0 = g_settings.backlight_bright;
    wheel(-4);
    CHECK("display: Brightness -4, and the backlight follows as it turns",
          g_settings.backlight_bright == b0 - 4 && hl_stats().backlight == b0 - 4);
    tap(HL_BTN_MENU, 500);
    tap(HL_BTN_MENU, 500);
    CHECK("display: MENU twice: the root", in_settings(SETTINGS_ROOT) && g_set_sel == 3);
}

/* ---- 3. Date & Time ------------------------------------------------------- */

static int settime_is(int y, int mo, int d, int h, int mi)
{
    return g_settime.year == y && g_settime.month == mo && g_settime.day == d &&
           g_settime.hour == h && g_settime.min == mi;
}

static void open_editor(const char *who, datetime_t *seed)
{
    tap(HL_BTN_SELECT, 500);
    int ok = now_local(seed);
    CHECK(who, ok && in_settings(SETTINGS_SETTIME) && g_settime.field == ST_YEAR &&
               settime_is(seed->year, seed->month, seed->day, seed->hour, seed->min));
}

static void datetime_cancel(void)
{
    set_row(5);                                     /* Date & Time */
    tap(HL_BTN_SELECT, 500);
    CHECK("dt: SELECT opens Date & Time on Set Date & Time",
          in_settings(SETTINGS_DATETIME) && g_set_sel == 0);
    datetime_t before;
    CHECK("dt: the clock is running", now_local(&before));

    datetime_t s;
    size_t at = log_mark();
    open_editor("dt: SELECT opens the editor on the year, seeded from the clock", &s);
    int use12 = !g_settings.time_24h;
    CHECK("dt: 12-hour mode has the AM/PM plate",
          g_settime.use_12h == use12 && settime_field_count(&g_settime) == 5 + use12);

    /* Year: clamps both ways (four years a flick at most). */
    for (int i = 0; i < 25; i++) flick(+1, 120);
    hl_run_ms(300);
    CHECK("dt: the year clamps at 2099", g_settime.year == SETTIME_YEAR_MAX && on_panel("2099"));
    for (int i = 0; i < 30; i++) flick(-1, 120);
    hl_run_ms(300);
    CHECK("dt: ...and at 2001", g_settime.year == SETTIME_YEAR_MIN && on_panel("2001"));
    wheel(1);
    CHECK("dt: one detent is one year (2002)", g_settime.year == 2002 && on_panel("2002"));
    CHECK("dt: the wheel moved nothing but the year",
          g_settime.field == ST_YEAR && g_settime.month == s.month && g_set_sel == 0);

    /* Month: wraps. */
    tap(HL_BTN_SELECT, 400);
    CHECK("dt: SELECT moves to the month", g_settime.field == ST_MONTH &&
                                           in_settings(SETTINGS_SETTIME));
    wheel(13 - s.month);
    CHECK("dt: the month wraps past December to January",
          g_settime.month == 1 && g_settime.year == 2002 && on_panel("Jan"));
    wheel(-1);
    CHECK("dt: ...and back to December", g_settime.month == 12 && on_panel("Dec"));

    /* Day: wraps within December's 31. */
    tap(HL_BTN_SELECT, 400);
    CHECK("dt: SELECT moves to the day", g_settime.field == ST_DAY);
    int d0 = g_settime.day;
    wheel(-d0);
    CHECK("dt: the day wraps below 1 to the 31st", g_settime.day == 31 && g_settime.month == 12);

    /* Hour. */
    tap(HL_BTN_SELECT, 400);
    CHECK("dt: SELECT moves to the hour", g_settime.field == ST_HOUR);
    int h0 = g_settime.hour;
    if (use12) {
        int pm = h0 >= 12;
        wheel(12 - h0 % 12);
        CHECK("dt: the 12-hour hour wraps to 12 inside its half-day",
              g_settime.hour == (pm ? 12 : 0));
    } else {
        wheel(24 - h0);
        CHECK("dt: the 24-hour hour wraps to 00", g_settime.hour == 0);
    }
    int h1 = g_settime.hour;

    /* Minute. */
    tap(HL_BTN_SELECT, 400);
    CHECK("dt: SELECT moves to the minute", g_settime.field == ST_MIN);
    wheel(-(g_settime.min + 1));
    CHECK("dt: the minute wraps below 0 to 59", g_settime.min == 59 && g_settime.hour == h1);

    if (use12) {
        tap(HL_BTN_SELECT, 400);
        CHECK("dt: SELECT moves to AM/PM", g_settime.field == ST_AMPM);
        wheel(1);
        CHECK("dt: one detent flips AM/PM", g_settime.hour == (h1 + 12) % 24);
        wheel(2);
        CHECK("dt: two more land back on it", g_settime.hour == (h1 + 12) % 24);
    }
    CHECK("dt: nothing written while editing", !log_has_since(at, "core: rtc set"));

    tap(HL_BTN_MENU, 500);
    CHECK("dt: MENU cancels back to Date & Time (not the root), row 0",
          in_settings(SETTINGS_DATETIME) && g_set_sel == 0 && !g_set_editing);
    datetime_t after;
    CHECK("dt: ...writing nothing", !log_has_since(at, "core: rtc set"));
    CHECK("dt: ...and the clock is where it was",
          now_local(&after) && after.year == before.year && after.month == before.month &&
          after.day == before.day && after.hour == before.hour);
}

static void datetime_commit(void)
{
    /* Time in Title on: idle, the status strip then shows the clock on every
     * Settings screen. Row 0 of Date & Time shows it whatever that setting. */
    set_row(2);
    tap(HL_BTN_SELECT, 500);
    CHECK("dt: Time in Title on", g_settings.time_in_title == 1);
    set_row(0);
    datetime_t now;
    char buf[16];
    now_local(&now);
    fmt_time(buf, &now);
    CHECK("dt: the strip shows the clock", strip_shows(buf));
    CHECK("dt: row 0 shows the clock, not \"Not set\"",
          rows_show(buf) && !on_panel("Not set"));
    if (T.fails) printf("  clock %s\n", buf);

    /* Keep the edit inside one minute and the set inside one hour. */
    if (now.sec > 45) {
        hl_run_ms(20000);
        now_local(&now);
    }
    if (now.min == 0) {
        hl_run_ms(60000);
        now_local(&now);
    }
    datetime_t s;
    size_t at = log_mark();
    open_editor("dt: SELECT opens the editor again, re-seeded", &s);
    int fields = settime_field_count(&g_settime);
    for (int f = ST_YEAR; f < ST_MIN; f++) tap(HL_BTN_SELECT, 300);
    CHECK("dt: four SELECTs: the minute", g_settime.field == ST_MIN);
    wheel(-1);
    CHECK("dt: minute -1", settime_is(s.year, s.month, s.day, s.hour, s.min - 1));
    for (int f = ST_MIN; f < fields - 1; f++) tap(HL_BTN_SELECT, 300);
    CHECK("dt: (on the last field)", g_settime.field == fields - 1 &&
                                     !log_has_since(at, "core: rtc set"));
    tap(HL_BTN_SELECT, 500);
    CHECK("dt: SELECT on the last field writes the RTC (rc 0)",
          log_has_since(at, " rc 00000000\n") && log_has_since(at, "core: rtc set "));
    CHECK("dt: ...and leaves for Date & Time, row 0",
          in_settings(SETTINGS_DATETIME) && g_set_sel == 0 && !g_set_editing);
    datetime_t c;
    CHECK("dt: the clock now reads the time entered",
          now_local(&c) && c.year == s.year && c.month == s.month && c.day == s.day &&
          c.hour == s.hour && c.min == s.min - 1 && c.sec <= 2);
    fmt_time(buf, &c);
    CHECK("dt: the strip shows it", strip_shows(buf));
    CHECK("dt: ...and so does row 0", rows_show(buf) && !on_panel("Not set"));
    if (T.fails) printf("  set %s\n", buf);

    /* And the strip keeps time from the new setting. */
    hl_run_ms(61000);
    now_local(&c);
    fmt_time(buf, &c);
    CHECK("dt: a minute later the strip shows the running time", strip_shows(buf));
    CHECK("dt: ...and so does row 0", rows_show(buf));
    if (T.fails) printf("  now %s\n", buf);
    tap(HL_BTN_MENU, 500);
    CHECK("dt: MENU: the root, cursor on Date & Time",
          in_settings(SETTINGS_ROOT) && g_set_sel == 5);
}

/* ---- 4. Now Playing ------------------------------------------------------- */

static void open_now_playing(void)
{
    tap(HL_BTN_MENU, 800);
    CHECK("np: MENU leaves Settings", g_scr_n == 1 && scr_cur() == SCR_MENU);
    wheel(MM_MUSIC - g_main_sel);
    tap(HL_BTN_SELECT, 600);
    wheel(MU_SONGS - g_music_sel);
    tap(HL_BTN_SELECT, 800);
    CHECK("np: Songs open", scr_cur() == SCR_SONGS);
    int k = find_song("Long Two");
    if (k != g_song_sel) wheel(k - g_song_sel);
    tap(HL_BTN_SELECT, 1500);
    CHECK("np: Long Two plays on Now Playing",
          scr_cur() == SCR_NOWPLAYING && player_active() && on_panel("Long Two") &&
          player_total_s() >= 85);
}

static void np_volume(void)
{
    hl_run_ms(2000);                                 /* plate down */
    int v0 = g_volume;
    CHECK("np: the volume is under the limit", v0 < 74 && hl_stats().volume == v0);
    wheel(1);
    CHECK("np: a detent is +1: g_volume, the record and the codec",
          g_volume == v0 + 1 && g_settings.volume == v0 + 1 &&
          hl_stats().volume == v0 + 1);

    /* The plate is up (1.5 s from the last detent): a second detent must
     * re-present it, not wait for the fade. */
    hl_on_present_rect(on_rect, NULL);
    g_plate_presents = 0;
    hl_scroll(1);
    hl_run_ms(400);
    CHECK("np: a detent while the plate is up re-presents the plate",
          g_plate_presents >= 1 && g_volume == v0 + 2);
    hl_on_present_rect(NULL, NULL);

    wheel(80 - g_volume);
    CHECK("np: the wheel stops at the Volume Limit (75)",
          g_volume == 75 && hl_stats().volume == 75);
    for (int i = 0; i < 8; i++) flick(-1, 120);
    hl_run_ms(300);
    CHECK("np: ...and at 0", g_volume == 0 && hl_stats().volume == 0);
    wheel(-1);
    CHECK("np: (still 0)", g_volume == 0);
    for (int i = 0; i < 4; i++) flick(+1, 250);
    hl_run_ms(2000);
    CHECK("np: flicks of 12 back up to 48", g_volume == 48 && hl_stats().volume == 48);
}

static void np_scrub(void)
{
    int v = g_volume;
    tap(HL_BTN_SELECT, 300);
    uint32_t e0 = elapsed_s();
    CHECK("scrub: a SELECT tap enters the scrubber, aimed at the elapsed time",
          np_scrubbing() && (g_scrub_target_s == e0 || g_scrub_target_s + 1 == e0));
    e0 = g_scrub_target_s;

    hl_wheel_clicks(4);
    hl_run_ms(200);
    CHECK("scrub: one detent aims 5 s on", g_scrub_target_s == e0 + SCRUB_STEP_S);
    CHECK("scrub: ...and leaves the volume alone", g_volume == v && hl_stats().volume == v);
    hl_scroll(2);
    hl_run_ms(300);
    CHECK("scrub: two more at rest: 15 s on", g_scrub_target_s == e0 + 3 * SCRUB_STEP_S);
    hl_run_ms(600);
    uint32_t e = elapsed_s();
    CHECK("scrub: the quiet commit seeks there",
          e >= e0 + 3 * SCRUB_STEP_S && e <= e0 + 3 * SCRUB_STEP_S + 1 && np_scrubbing());

    for (int i = 0; i < 4; i++) flick(-1, 80);
    hl_run_ms(100);
    CHECK("scrub: spun back past the start, the target clamps at 0", g_scrub_target_s == 0);
    hl_run_ms(700);
    CHECK("scrub: ...and the seek lands there", elapsed_s() <= 1);

    hl_run_ms(4200);
    CHECK("scrub: 4 s quiet: the scrubber lapses", !np_scrubbing());
    v = g_volume;
    wheel(1);
    CHECK("scrub: ...and the wheel is volume again",
          g_volume == v + 1 && hl_stats().volume == v + 1 && elapsed_s() < 10);

    hl_run_ms(1600);                                 /* plate down */
    tap(HL_BTN_SELECT, 300);
    CHECK("scrub: a tap enters it again", np_scrubbing());
    uint32_t total = player_total_s();
    for (int i = 0; i < 6; i++) flick(+1, 80);
    hl_run_ms(100);
    CHECK("scrub: spun past the end, the target clamps at the length",
          total > 0 && g_scrub_target_s == total);
    hl_run_ms(150);                                  /* < SCRUB_COMMIT_US quiet */
    hl_scroll(-2);
    hl_run_ms(300);
    CHECK("scrub: two detents back from the end: length - 10",
          g_scrub_target_s == total - 2 * SCRUB_STEP_S);
    hl_run_ms(600);
    e = elapsed_s();
    CHECK("scrub: ...seeked there, still Long Two",
          e >= total - 2 * SCRUB_STEP_S && e <= total - 2 * SCRUB_STEP_S + 1 &&
          on_panel("Long Two"));
    tap(HL_BTN_SELECT, 300);
    CHECK("scrub: a second SELECT tap leaves it", !np_scrubbing());
    v = g_volume;
    wheel(1);
    CHECK("scrub: ...and the wheel is volume again",
          g_volume == v + 1 && hl_stats().volume == v + 1);

    tap(HL_BTN_MENU, 500);
    CHECK("np: MENU goes back to Songs, still playing",
          scr_cur() == SCR_SONGS && player_active() && hl_stats().audio_running);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/np_settings_input_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);

    CHECK("image: the pre-allocated files' extents are known", load_ranges(argv[2]) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        unlink(disk);
        return 2;
    }
    CHECK("boot: reached the browser", strstr(hl_log(), "core: entering browser") != NULL);

    wheel(MM_SETTINGS - g_main_sel);
    tap(HL_BTN_SELECT, 600);
    CHECK("settings: open on the root", in_settings(SETTINGS_ROOT));

    sound();
    display();
    datetime_cancel();
    datetime_commit();
    open_now_playing();
    np_volume();
    np_scrub();

    hl_stats_t st = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", st.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) fputs(hl_log(), stderr);
    printf("np-settings-input: done at fake time %.1f s\n", (double)st.now_us / 1e6);
    unlink(disk);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}
