/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/row_hold_test.c — holding SELECT on every kind of list row.
 *
 * SELECT on a list row is a tap (act) or a hold (add to On-The-Go, or remove
 * from it), told apart by press length: the down-edge records the screen and
 * row (rowsel_arm), a per-pass block in the main loop decides, and the press
 * is dropped (rowsel_drop) when the screen, the list or Hold changes under
 * it. otg-walk holds an album row, a track row and an On-The-Go row once
 * each; this holds every row kind the long press means something on, and the
 * ones it must mean nothing on, and judges each by what On-The-Go then holds
 * (which songs, in which order), the banner, and where the cursor is left.
 *
 * The disk is the headless disk built with --stray: Mono Mix's folder also
 * holds Stray.flac, a file the index has no record of
 * (tests/scripts/make_headless_disk.py).
 *
 * Every case is its own power-on on a fresh copy of the disk:
 *
 *   songs     Music > Songs: a hold adds the row's song (not the song at
 *             that index of the library); a press whose wheel moves the
 *             cursor mid-hold adds the row it began on and leaves the cursor
 *             where the wheel put it; a tap shorter than a sampler tick
 *             still plays (the arm's own feed)
 *   playlist  Mixed: a hold adds the row's song; on Bonus (a file the index
 *             does not know) "Not in the library" and nothing added; on a
 *             saved slot's Delete Playlist row nothing at all (not a stale
 *             song, not the delete confirmation)
 *   browser   Artists > Low Rate: a hold on "All Songs" adds nothing and
 *             stays; in Mono Mix, on Stray "Not in the library"; a track
 *             press whose MENU leaves the tracklist mid-hold adds nothing
 *   search    "LO": a hold on a song hit adds that song, with the wheel moved
 *             mid-hold (the recorded hit acts, the cursor stays moved); on an
 *             album hit nothing, and the hit is not opened
 *   otg       remove the last row (the cursor clamps to the new last row,
 *             a part-detent of wheel before the hold is forgotten, the change
 *             is committed to COREOTG.DAT), then every row (the cursor on
 *             Clear Playlist, not the greyed Save)
 *   drop      a Songs press with MENU before the hold time; one with Hold
 *             switched on under it and the finger lifted before Hold comes
 *             off; the same, then the screen left to go dark and woken with
 *             a long SELECT: none adds or plays. And a finger KEPT on Select
 *             through Hold-off, which the wheel reports as a fresh press
 *             (hal/hw/clickwheel.c): that is a new hold, and it adds
 *
 * Throughout: no write outside the pre-allocated files, no panic.
 *
 * Usage: row_hold_test DISK.img DISK.img.ranges [SHOT_DIR]
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

static xfail_ctx T = { "row-hold", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

static const char *g_shots;
static const char *g_case = "";
static int g_shot_n;

/* ---- helpers (as otg_walk_test.c's) -------------------------------------- */

static void shot(const char *name)
{
    if (!g_shots) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/row_hold_%s_%02d_%s.bmp",
             g_shots, g_case, g_shot_n++, name);
    (void)hl_write_bmp(path);
}

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int on_screen(const char *s) { return hl_find_text(hl_panel(), s, NULL); }
static int header(const char *s) { return hl_find_text_in(hl_panel(), s, 0, 40, NULL); }
static int in_list(const char *s) { return hl_find_text_in(hl_panel(), s, 40, HL_H, NULL); }
/* `s` is on the highlighted row. Searched a band at a time: the faces are
 * tried in turn, so over the whole list "Songs" would be found first in the
 * plain face inside "Shuffle Songs" and the highlighted (bold) row above it
 * never looked at. */
static int selected(const char *s)
{
    for (int y0 = 40; y0 < HL_H; y0 += 8) {
        hl_text_hit_t h;
        int y1 = y0 + 40 < HL_H ? y0 + 40 : HL_H;
        if (hl_find_text_in(hl_panel(), s, y0, y1, &h) && lum(h.ink) > lum(h.bg)) {
            return 1;
        }
    }
    return 0;
}
static void tap(unsigned btn) { hl_tap(btn, 500); }

/* Wheel to the top of a list, then down a row at a time to `s`. */
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

/* Hold Select past SEL_HOLD_US (450 ms). Returns with the confirmation
 * banner (900 ms) still up. */
static void hold_select(void)
{
    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(800);
}

/* Let a banner run out, so the next one is a new one. */
static void settle(void) { hl_run_ms(1200); }

/* Any of the On-The-Go banners. */
static int any_banner(void)
{
    return on_screen("Added") || on_screen("Removed") ||
           on_screen("Not in the library");
}

static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
}
typedef struct { size_t from; const char *needle; } log_wait_t;
static int pred_log(void *a)
{
    const log_wait_t *w = a;
    return log_has_since(w->from, w->needle);
}
static int pred_header(void *a) { return header((const char *)a); }
static int pred_dark(void *a) { (void)a; return hl_stats().backlight == 0; }

/* The list rows, top to bottom, are exactly `want[0..n)` in that order (each
 * one's ink box lower than the last; rows are 32 px). */
static int rows_in_order(const char *const *want, int n)
{
    int last_y = -1;
    for (int i = 0; i < n; i++) {
        hl_text_hit_t h;
        if (!hl_find_text_in(hl_panel(), want[i], 40, HL_H, &h)) return 0;
        if (h.y <= last_y + 16) return 0;
        last_y = h.y;
    }
    return 1;
}

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

static _Noreturn void session_end(void)
{
    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: fake time %.1f s\n", g_case, (double)s.now_us / 1e6);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);                     /* the firmware thread is frozen mid-wait */
}

/* ---- navigation ------------------------------------------------------------- */

static void to_music(void)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    CHECK("nav: main menu", select_row("Music"));
    tap(HL_BTN_SELECT);
}

/* Music > `row`, waiting for the header `hdr`. */
static void to_music_row(const char *row, const char *hdr)
{
    to_music();
    char l[96];
    snprintf(l, sizeof l, "nav: Music > %s", row);
    CHECK(l, select_row(row));
    tap(HL_BTN_SELECT);
    snprintf(l, sizeof l, "nav: the %s screen", hdr);
    CHECK(l, hl_run_until(pred_header, (void *)hdr, 250, 5000));
}

/* Playlists > `name`, opened. */
static void open_playlist(const char *name)
{
    char l[96];
    snprintf(l, sizeof l, "nav: Playlists > %s", name);
    CHECK(l, select_row(name));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    snprintf(l, sizeof l, "nav: the %s screen", name);
    CHECK(l, header(name));
}

/* Playlists > On-The-Go. Opening it probes the slot files, so it may wait on
 * the drive. */
static void open_otg(void)
{
    to_music_row("Playlists", "Playlists");
    /* The pinned first row: "On-The-Go" alone would also match a saved
     * "On-The-Go 1" further down. */
    hl_scroll(-16);
    hl_run_ms(16 * 150 + 400);
    CHECK("nav: the On-The-Go row", selected("On-The-Go") && !selected("On-The-Go 1"));
    tap(HL_BTN_SELECT);
    CHECK("nav: the On-The-Go screen", hl_run_until(pred_header, "On-The-Go", 250, 5000));
}

/* Every title a row on this disk can carry: what On-The-Go must NOT show is
 * whichever of these is not wanted. */
static const char *const TITLES[] = {
    "Allegro", "Andante", "Sine", "Long One", "Long Two",
    "Mono Flac", "Mono Mp3", "Bonus", "Stray",
};

/* On-The-Go holds exactly `want[0..n)`, in that order, every one bound. */
static void expect_otg(const char *what, const char *const *want, int n)
{
    open_otg();
    shot("otg");
    char l[160];
    const char *rows[8] = { "Save Playlist" };
    for (int i = 0; i < n; i++) rows[i + 1] = want[i];
    snprintf(l, sizeof l, "%s: On-The-Go holds the %d, in order", what, n);
    CHECK(l, rows_in_order(rows, n + 1));
    for (size_t t = 0; t < sizeof TITLES / sizeof TITLES[0]; t++) {
        int wanted = 0;
        for (int i = 0; i < n; i++) wanted |= !strcmp(TITLES[t], want[i]);
        if (wanted) continue;
        snprintf(l, sizeof l, "%s: ...and not %s", what, TITLES[t]);
        CHECK(l, !in_list(TITLES[t]));
    }
    snprintf(l, sizeof l, "%s: ...every entry bound to a song", what);
    CHECK(l, !in_list("Not on this iPod"));
}

/* Hold on the selected row, which must add one song: the banner, the screen
 * `hdr` still up with `row` still the cursor, nothing playing. */
static void hold_adds(const char *hdr, const char *row)
{
    char l[160];
    snprintf(l, sizeof l, "%s: select %s", hdr, row);
    CHECK(l, select_row(row));
    hold_select();
    shot("added");
    snprintf(l, sizeof l, "%s: holding Select on %s adds it (banner)", hdr, row);
    CHECK(l, on_screen("Added to On-The-Go"));
    settle();                      /* the banner covers the header */
    snprintf(l, sizeof l, "%s: ...on the same screen, %s still the cursor", hdr, row);
    CHECK(l, header(hdr) && selected(row));
    snprintf(l, sizeof l, "%s: ...and plays nothing", hdr);
    CHECK(l, !hl_stats().audio_running);
}

/* Hold on `row`, which the library does not know: the refusal banner. */
static void hold_not_in_library(const char *hdr, const char *row)
{
    char l[160];
    snprintf(l, sizeof l, "%s: select %s", hdr, row);
    CHECK(l, select_row(row));
    hold_select();
    shot("not_in_library");
    snprintf(l, sizeof l, "%s: holding Select on %s says \"Not in the library\"",
             hdr, row);
    CHECK(l, on_screen("Not in the library"));
    snprintf(l, sizeof l, "%s: ...adds nothing (no Added banner)", hdr);
    CHECK(l, !on_screen("Added"));
    settle();
    snprintf(l, sizeof l, "%s: ...on the same screen, playing nothing", hdr);
    CHECK(l, header(hdr) && selected(row) && !hl_stats().audio_running);
}

/* ---- the cases ---------------------------------------------------------------- */

static void case_songs(void)
{
    to_music_row("Songs", "Songs");
    /* Songs is in title order; the library (g_songs) is in folder order, so
     * the fourth title row is not the library's fourth song. */
    hold_adds("Songs", "Long Two");

    /* The row the press began on is the row that acts: the wheel rocks a
     * detent while the thumb is down, the cursor follows it, the add does
     * not. */
    CHECK("songs: select Mono Flac", select_row("Mono Flac"));
    hl_press(HL_BTN_SELECT, 1000);
    hl_run_ms(150);
    hl_scroll(1);
    hl_run_ms(700);
    shot("rocked");
    CHECK("songs: a hold with the wheel moved adds (banner)",
          on_screen("Added to On-The-Go"));
    CHECK("songs: ...the cursor where the wheel left it (Mono Mp3)",
          selected("Mono Mp3") && !selected("Mono Flac"));
    hl_run_ms(1500);

    const char *want[] = { "Long Two", "Mono Flac" };
    expect_otg("songs", want, 2);

    /* A tap whose press and release both fall between two samples of the
     * live buttons: only the down-edge's own feed (rowsel_arm) times it, and
     * it must still play the row. */
    to_music_row("Songs", "Songs");
    CHECK("songs: select Andante", select_row("Andante"));
    hl_press(HL_BTN_SELECT, 1);
    hl_run_until(pred_header, "Now Playing", 100, 4000);
    hl_run_ms(500);
    shot("short_tap");
    CHECK("songs: a 1 ms tap plays the row (Now Playing, Andante)",
          header("Now Playing") && on_screen("Andante"));
    CHECK("songs: ...the DAC is running", hl_stats().audio_running);
}

static void case_playlist(void)
{
    to_music_row("Playlists", "Playlists");
    open_playlist("Mixed");
    /* Mixed's rows: Allegro, Mono Mp3, Long One, Mono Flac, Bonus. */
    hold_adds("Mixed", "Long One");
    hold_not_in_library("Mixed", "Bonus");
    const char *want[] = { "Long One" };
    expect_otg("playlist", want, 1);

    /* Save it into a slot: a playlist whose last row is Delete Playlist. */
    CHECK("playlist: select Save Playlist", select_row("Save Playlist"));
    size_t mark = hl_log_len();
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    CHECK("playlist: saved into slot 1",
          log_has_since(mark, "core: otg slot save rc 00000000 wrote 1 of 1"));
    /* Mixed again, so the rows past the slot's last track hold Mixed's songs:
     * the Delete row's index is one of them. */
    open_playlist("Mixed");
    tap(HL_BTN_MENU);
    open_playlist("On-The-Go 1");
    {
        const char *rows[] = { "Long One", "Delete Playlist" };
        CHECK("playlist: the slot is Long One, then Delete Playlist",
              rows_in_order(rows, 2));
    }
    CHECK("playlist: select Delete Playlist", select_row("Delete Playlist"));
    mark = hl_log_len();
    hold_select();
    shot("delete_row_hold");
    CHECK("playlist: holding Select on Delete Playlist shows no banner", !any_banner());
    CHECK("playlist: ...does not ask to delete", !in_list("Delete? Select again"));
    hl_run_ms(1500);
    CHECK("playlist: ...erases nothing", !log_has_since(mark, "core: otg slot erase"));
    CHECK("playlist: ...still on the slot", header("On-The-Go 1") && in_list("Long One"));
    expect_otg("playlist delete row", NULL, 0);
}

static void case_browser(void)
{
    to_music_row("Artists", "Artists");
    CHECK("browser: the Low Rate artist", select_row("Low Rate"));
    tap(HL_BTN_SELECT);
    hl_run_ms(500);
    CHECK("browser: Low Rate's albums", header("Low Rate") && in_list("Mono Mix"));
    CHECK("browser: select All Songs", select_row("All Songs"));
    hold_select();
    shot("all_songs_hold");
    CHECK("browser: holding Select on All Songs shows no banner", !any_banner());
    CHECK("browser: ...and stays on the album list",
          header("Low Rate") && selected("All Songs") && in_list("Mono Mix"));
    settle();

    CHECK("browser: select Mono Mix", select_row("Mono Mix"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    CHECK("browser: the tracklist, Stray last",
          in_list("Mono Flac") && in_list("Mono Mp3") && in_list("Stray"));
    hold_not_in_library("Albums", "Stray");
    hold_adds("Albums", "Mono Mp3");

    /* A track press that MENU takes out of the tracklist before the hold
     * time: the browser's other half (the album list), so the same screen. */
    CHECK("browser: select Mono Flac", select_row("Mono Flac"));
    hl_press(HL_BTN_SELECT, 1500);
    hl_run_ms(200);
    hl_press(HL_BTN_MENU, 60);
    hl_run_ms(2500);
    shot("menu_mid_press");
    CHECK("browser: MENU mid-press leaves the tracklist",
          in_list("All Songs") && in_list("Mono Mix") && !in_list("Stray"));
    CHECK("browser: ...the press adds nothing, plays nothing",
          !any_banner() && !hl_stats().audio_running);

    const char *want[] = { "Mono Mp3" };
    expect_otg("browser", want, 1);
}

/* The ring: A-Z, 0-9, SPACE, DEL, DONE; a fresh boot starts it on 'A'. */
#define RING_N    39
#define CELL_DONE 38
static int g_cell;

static void ring_to(int cell)
{
    int d = (cell - g_cell + RING_N) % RING_N;
    if (d > RING_N / 2) d -= RING_N;
    if (d) {
        hl_scroll(d);
        hl_run_ms((uint32_t)(d < 0 ? -d : d) * 150 + 400);
    }
    g_cell = cell;
}

static void case_search(void)
{
    to_music_row("Search", "Search");
    g_cell = 0;
    for (const char *c = "LO"; *c; c++) {
        ring_to(*c - 'A');
        tap(HL_BTN_SELECT);
    }
    ring_to(CELL_DONE);
    tap(HL_BTN_SELECT);
    shot("results");
    CHECK("search: the results",
          header("Search") && in_list("Long One") && in_list("Long Two") &&
          in_list("Long Player"));

    /* The recorded hit acts; the cursor is where the wheel left it. */
    CHECK("search: select the Long Two hit", select_row("Long Two"));
    hl_press(HL_BTN_SELECT, 1000);
    hl_run_ms(150);
    hl_scroll(-1);
    hl_run_ms(700);
    shot("song_hold");
    CHECK("search: holding Select on a song hit adds it (banner)",
          on_screen("Added to On-The-Go"));
    settle();
    CHECK("search: ...the cursor where the wheel left it (Long One)",
          header("Search") && selected("Long One") && !selected("Long Two"));
    CHECK("search: ...and plays nothing", !hl_stats().audio_running);

    CHECK("search: select the Long Player hit", select_row("Long Player"));
    hold_select();
    shot("album_hold");
    CHECK("search: holding Select on an album hit shows no banner", !any_banner());
    CHECK("search: ...and does not open it",
          header("Search") && selected("Long Player"));
    settle();

    const char *want[] = { "Long Two" };
    expect_otg("search", want, 1);
}

static void case_otg(void)
{
    to_music_row("Songs", "Songs");
    hold_adds("Songs", "Allegro");
    hold_adds("Songs", "Sine");
    size_t mark = hl_log_len();
    hold_adds("Songs", "Mono Mp3");

    const char *three[] = { "Allegro", "Sine", "Mono Mp3" };
    expect_otg("otg", three, 3);
    /* The adds reach COREOTG.DAT once the drive is turning (opening On-The-Go
     * probes the slot files): nothing is left pending before the remove, so
     * the remove's own commit is the remove's doing. */
    log_wait_t w = { mark, "core: otg save rc 00000000 seq " };
    CHECK("otg: the adds are committed to COREOTG.DAT",
          hl_run_until(pred_log, &w, 500, 60000));

    /* The last row goes: the cursor clamps to the new last row, and a part
     * of a detent turned before the hold does not carry over. */
    CHECK("otg: select Mono Mp3 (the last row)", select_row("Mono Mp3"));
    hl_wheel_clicks(-2);
    hl_run_ms(300);
    CHECK("otg: half a detent does not move the cursor", selected("Mono Mp3"));
    mark = hl_log_len();
    hold_select();
    shot("removed_last");
    CHECK("otg: holding Select on a row removes it (banner)", on_screen("Removed"));
    CHECK("otg: ...the count after it", on_screen("2 SONGS"));
    settle();
    {
        const char *rows[] = { "Save Playlist", "Allegro", "Sine" };
        CHECK("otg: the other two stay, in order", rows_in_order(rows, 3));
    }
    CHECK("otg: Mono Mp3 is gone", !in_list("Mono Mp3"));
    CHECK("otg: the cursor clamps to the new last row (Sine)", selected("Sine"));
    hl_wheel_clicks(-2);
    hl_run_ms(300);
    shot("half_detent");
    CHECK("otg: half a detent after the remove does not move the cursor",
          selected("Sine") && !selected("Allegro"));
    w.from = mark;
    CHECK("otg: the remove is committed to COREOTG.DAT",
          hl_run_until(pred_log, &w, 500, 60000));

    /* Every row goes: the empty state, the cursor on Clear Playlist. */
    CHECK("otg: select Allegro", select_row("Allegro"));
    hold_select();
    CHECK("otg: Allegro removed (banner)", on_screen("Removed"));
    settle();
    CHECK("otg: the cursor on Sine, now the only row", selected("Sine"));
    hold_select();
    shot("emptied");
    CHECK("otg: Sine removed (banner)", on_screen("Removed"));
    settle();
    shot("empty");
    CHECK("otg: no entries left",
          !in_list("Allegro") && !in_list("Sine") && !in_list("Mono Mp3"));
    CHECK("otg: the cursor on Clear Playlist, not the greyed Save Playlist",
          selected("Clear Playlist") && !selected("Save Playlist"));
}

static void case_drop(void)
{
    to_music_row("Songs", "Songs");

    /* MENU before the hold time: the screen goes, so does the press. */
    CHECK("drop: select Long Two", select_row("Long Two"));
    hl_press(HL_BTN_SELECT, 1500);
    hl_run_ms(200);
    hl_press(HL_BTN_MENU, 60);
    hl_run_ms(2500);
    shot("menu");
    CHECK("drop: MENU mid-press goes back to Music", header("Music"));
    CHECK("drop: ...the press adds nothing, plays nothing",
          !any_banner() && !hl_stats().audio_running);

    /* Hold on under the press, the finger lifted while it is on, Hold off:
     * the press is void — no hold, and no tap on the release. */
    CHECK("drop: Songs again", select_row("Songs"));
    tap(HL_BTN_SELECT);
    CHECK("drop: select Long Two again", select_row("Long Two"));
    hl_press(HL_BTN_SELECT, 600);
    hl_run_ms(200);
    hl_set_hold(1);
    hl_run_ms(700);                /* the finger lifts at 600 ms, Hold on */
    hl_set_hold(0);
    hl_run_ms(1000);
    shot("hold_switch");
    CHECK("drop: a press lifted under Hold adds nothing", !on_screen("Added"));
    hl_run_ms(1500);
    CHECK("drop: ...and plays nothing",
          header("Songs") && selected("Long Two") && !hl_stats().audio_running);

    /* The same, then the screen left to go dark: the SELECT that wakes it
     * only lights it (keyhold_void), held well past the hold time. */
    CHECK("drop: select Long Two (dark)", select_row("Long Two"));
    hl_press(HL_BTN_SELECT, 600);
    hl_run_ms(200);
    hl_set_hold(1);
    hl_run_ms(700);
    hl_set_hold(0);
    hl_run_ms(500);
    CHECK("drop: the backlight times out",
          hl_run_until(pred_dark, NULL, 1000, 90000));
    hl_press(HL_BTN_SELECT, 1500);
    hl_run_ms(1000);
    shot("wake");
    CHECK("drop: the waking SELECT adds nothing", !on_screen("Added"));
    hl_run_ms(1500);
    CHECK("drop: ...and plays nothing",
          header("Songs") && selected("Long Two") && !hl_stats().audio_running);
    expect_otg("drop", NULL, 0);

    /* What the hardware does with a finger KEPT on Select through Hold-off
     * (hal/hw/clickwheel.c): the wheel block restarts and reports the button
     * as a fresh press, so it is a new long press — and it adds. */
    to_music_row("Songs", "Songs");
    CHECK("drop: select Long Two (kept)", select_row("Long Two"));
    hl_press(HL_BTN_SELECT, 2500);
    hl_run_ms(200);
    hl_set_hold(1);
    hl_run_ms(200);
    hl_set_hold(0);
    hl_run_ms(1000);
    shot("kept");
    CHECK("drop: a finger kept through Hold-off is a new hold: it adds",
          on_screen("Added to On-The-Go"));
    hl_run_ms(2500);
    CHECK("drop: ...and the release plays nothing",
          header("Songs") && !hl_stats().audio_running);
    const char *want[] = { "Long Two" };
    expect_otg("drop kept", want, 1);
}

typedef struct {
    const char *name;
    void (*run)(void);
} case_t;

static const case_t CASES[] = {
    { "songs",    case_songs },
    { "playlist", case_playlist },
    { "browser",  case_browser },
    { "search",   case_search },
    { "otg",      case_otg },
    { "drop",     case_drop },
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges [SHOT_DIR]\n", argv[0]);
        return 2;
    }
    g_shots = argc > 3 ? argv[3] : NULL;
    const char *only = getenv("ROW_HOLD_CASE");
    int ok = 1;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (only && strcmp(only, CASES[i].name)) continue;
        char disk[] = "/tmp/row_hold_XXXXXX";
        int fd = mkstemp(disk);
        if (fd < 0 || copy_disk(argv[1], disk) != 0) {
            fprintf(stderr, "cannot copy %s\n", argv[1]);
            return 2;
        }
        close(fd);
        fflush(NULL);
        pid_t p = fork();
        if (p == 0) {
            g_case = CASES[i].name;
            T.suite = CASES[i].name;
            CHECK("image: the pre-allocated files' extents are known",
                  load_ranges(argv[2]) >= 3);
            if (hl_boot(disk, 12000) != 0) {
                fprintf(stderr, "hl_boot failed\n");
                _exit(2);
            }
            CHECK("boot: the live list starts empty",
                  strstr(hl_log(), "core: otg load 0 ") != NULL);
            CASES[i].run();
            session_end();
        }
        int st = 0;
        waitpid(p, &st, 0);
        unlink(disk);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fprintf(stderr, "row-hold: %s FAILED\n", CASES[i].name);
            ok = 0;
        }
    }
    return ok ? 0 : 1;
}
