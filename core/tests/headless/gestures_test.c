/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/gestures_test.c — run_ui's button gestures (app/gestures.c),
 * each judged by the firmware state it leaves (read while frozen) and the
 * panel.
 *
 * The walks press these buttons on their way somewhere, so most gestures were
 * reached but not judged: a RIGHT hold that never sought, a MENU hold that
 * stayed put, a Hold banner never armed, a press under Hold that acted, the
 * charging modal that a press could not dismiss — each passed the suite. One
 * power-on of the stock disk, in order:
 *
 *   transport  Long Player played from the album list. RIGHT there jumps to
 *              Now Playing and does not skip; on Now Playing a RIGHT tap
 *              skips, a LEFT tap early in a track goes back; a RIGHT hold
 *              seeks forward by more than the time it was held, and a LEFT
 *              tap late in a track restarts it; a MENU hold is the main
 *              menu, from three screens deep
 *   select     Now Playing's SELECT: a tap is the scrubber; a wheel turn aims
 *              it and, after 400 ms of quiet, ONE seek to the target; 4 s of
 *              quiet more and it lapses; a hold is the queue, cursor on the
 *              playing row; a press MENU took away from Now Playing does
 *              nothing when it is held on or let go
 *   hold       the Hold switch: the lock and the "Locked" banner; the wheel
 *              and a press under Hold do nothing; switched off in the dark,
 *              the panel lights and STAYS lit (the edge is an input)
 *   banners    a wheel turn ends the On-The-Go banner early (and is applied)
 *   wake       a press clicks; a wheel turn on a dimmed screen relights it at
 *              full brightness
 *   modal      a press on the charging screen dismisses it and nothing else
 *   track end  a track ending under the scrubber takes the scrubber with it
 *
 * Usage: gestures_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "player/player.h"
#include "app/state.h"              /* g_lock_flash, g_settings             */
#include "app/nav.h"                /* scr_cur, g_scr, g_scr_n              */
#include "app/lists.h"              /* g_music_sel, MU_*, g_queue_sel       */
#include "app/library.h"            /* g_albums                             */
#include "app/albums.h"             /* g_albumview, g_br_sel, g_dir_depth   */
#include "app/otg.h"                /* g_otg, g_otg_flash                   */
#include "app/nowplaying.h"         /* g_sel_pending, scrubber state        */
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "gestures", 0, 0, 0 };

/* The playing position. player_elapsed_s() reads the timer, so fake time is
 * held across the call (hl_hold_time). */
static uint32_t elapsed(void)
{
    hl_hold_time(1);
    uint32_t e = player_elapsed_s();
    hl_hold_time(0);
    return e;
}
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* ---- helpers (as browse_input_test.c's) ------------------------------------ */

static int on_screen(const char *s) { return hl_find_text(hl_panel(), s, NULL); }
static void wheel(int rows)
{
    if (rows == 0) return;
    hl_scroll(rows);
    hl_run_ms((uint32_t)(rows < 0 ? -rows : rows) * 150u + 400u);
}
static void tap(unsigned btn, uint32_t settle) { hl_tap(btn, settle); }
static int top_is(screen_t below, screen_t top)
{
    return g_scr_n >= 2 && scr_cur() == top && g_scr[g_scr_n - 2] == below;
}
static int pred_dark(void *a) { (void)a; return hl_stats().backlight == 0; }
static int pred_dim(void *a)
{
    (void)a;
    int b = hl_stats().backlight;
    return b > 0 && b < g_settings.backlight_bright;
}
static int pred_second(void *a) { (void)a; return player_queue_current() == 1; }
static int lit_full(void) { return hl_stats().backlight == g_settings.backlight_bright; }
static int playing(void) { return hl_stats().audio_running && !player_paused(); }
static int pred_playing(void *a) { (void)a; return playing(); }

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

/* MENU taps down to the main menu, then SELECT: the Music menu. */
static int to_music(void)
{
    for (int i = 0; i < 8 && g_scr_n > 1; i++) tap(HL_BTN_MENU, 400);
    if (scr_cur() != SCR_MENU) return 0;
    tap(HL_BTN_SELECT, 500);
    return top_is(SCR_MENU, SCR_MUSIC);
}

/* From the Music menu: Albums, the cursor on the album whose folder names
 * `album`. */
static int to_album_row(const char *album)
{
    wheel(MU_ALBUMS - g_music_sel);
    tap(HL_BTN_SELECT, 800);
    if (!top_is(SCR_MUSIC, SCR_BROWSER) || g_dir_depth != 0) return 0;
    int k = -1;
    for (int i = 0; i < g_albumview_n; i++) {
        if (strstr(g_albums[g_albumview[i]].folder, album)) { k = i; break; }
    }
    if (k < 0) return 0;
    wheel(k - g_br_sel);
    return g_br_sel == k;
}

/* ---- transport: RIGHT / LEFT / MENU by press length ------------------------- */

static void transport(void)
{
    CHECK("transport: Albums, on Long Player", to_music() && to_album_row("Long Player"));
    tap(HL_BTN_PLAY, 1500);
    CHECK("transport: Long One playing, on Now Playing",
          playing() && player_queue_current() == 0 && scr_cur() == SCR_NOWPLAYING);
    tap(HL_BTN_MENU, 800);
    CHECK("transport: MENU is the album list, still playing",
          scr_cur() == SCR_BROWSER && playing());
    int below = (int)scr_cur();

    /* Off the player screens RIGHT is the jump to Now Playing, pushed over the
     * list, and never a skip. */
    tap(HL_BTN_RIGHT, 1500);
    CHECK("transport: RIGHT from the list is Now Playing, pushed over it",
          scr_cur() == SCR_NOWPLAYING && g_scr_n >= 2 &&
          (int)g_scr[g_scr_n - 2] == below);
    CHECK("transport: ...and no skip", player_queue_current() == 0);

    tap(HL_BTN_RIGHT, 1500);
    CHECK("transport: RIGHT tap on Now Playing skips", player_queue_current() == 1 &&
          playing());
    tap(HL_BTN_LEFT, 1500);
    CHECK("transport: LEFT tap early in a track goes back", player_queue_current() == 0);

    /* A 3 s hold: past GESTURE_SEEK_HOLD_US it aims a step per 250 ms tick,
     * accelerating, and the release commits one seek. */
    uint32_t el0 = elapsed();
    hl_press(HL_BTN_RIGHT, 3000);
    hl_run_ms(3800);
    uint32_t el1 = elapsed();
    CHECK("transport: a RIGHT hold seeks forward, past real time",
          player_queue_current() == 0 && el1 >= el0 + 8);
    printf("transport: RIGHT held 3 s: %u s -> %u s\n", (unsigned)el0, (unsigned)el1);
    tap(HL_BTN_LEFT, 800);
    CHECK("transport: LEFT late in a track restarts it",
          player_queue_current() == 0 && elapsed() < 3 && playing());

    /* MENU held: its tap pops at the down-edge, the hold is home. */
    CHECK("transport: three deep", g_scr_n >= 3 && scr_cur() == SCR_NOWPLAYING);
    hl_press(HL_BTN_MENU, 1500);
    hl_run_ms(2000);
    CHECK("transport: a MENU hold is the main menu", g_scr_n == 1 && scr_cur() == SCR_MENU);
}

/* ---- select: Now Playing's SELECT and the scrubber ------------------------- */

static void select_np(void)
{
    tap(HL_BTN_RIGHT, 1200);          /* main menu -> Now Playing */
    CHECK("select: Now Playing", scr_cur() == SCR_NOWPLAYING && playing());
    tap(HL_BTN_SELECT, 200);
    CHECK("select: a tap is the scrubber", np_scrubbing() && !g_sel_pending);

    uint32_t el0 = elapsed();
    wheel(3);                         /* aims; 850 ms with the wheel at rest   */
    hl_run_ms(300);
    uint32_t tgt = g_scrub_target_s;
    CHECK("select: the wheel aimed ahead", tgt >= el0 + 10);
    CHECK("select: ...and 400 ms of quiet sought it, once",
          !g_scrub_dirty && elapsed() + 1 >= tgt && elapsed() <= tgt + 2);
    CHECK("select: still scrubbing", np_scrubbing());
    hl_run_ms(4500);
    CHECK("select: 4 s of quiet and the scrubber lapses", !np_scrubbing());

    tap(HL_BTN_RIGHT, 1500);          /* Long Two: the queue cursor is not 0 */
    CHECK("select: Long Two", player_queue_current() == 1);
    hl_press(HL_BTN_SELECT, 800);
    hl_run_ms(1200);
    CHECK("select: a hold is the queue, on the playing row",
          scr_cur() == SCR_QUEUE && g_queue_sel == 1 && !np_scrubbing());
    tap(HL_BTN_MENU, 800);
    CHECK("select: MENU is Now Playing again", scr_cur() == SCR_NOWPLAYING);

    /* SELECT goes down on Now Playing, MENU takes Now Playing away, SELECT is
     * held on past the hold time, then let go: the press is dropped. */
    hl_press(HL_BTN_SELECT, 1200);
    hl_run_ms(100);
    tap(HL_BTN_MENU, 200);
    hl_run_ms(1600);
    CHECK("select: a press MENU took away is dropped",
          scr_cur() != SCR_NOWPLAYING && scr_cur() != SCR_QUEUE && !np_scrubbing() &&
          !g_sel_pending);
}

/* ---- hold: the Hold switch ---------------------------------------------- */

static void hold_switch(void)
{
    CHECK("hold: the Music menu, playing", to_music() && playing());

    int sel0 = g_music_sel;
    hl_set_hold(1);
    hl_run_ms(200);
    CHECK("hold: switched on, the input is locked", g_locked);
    CHECK("hold: the Locked banner", g_lock_flash.armed && on_screen("Locked"));
    hl_run_ms(1200);
    CHECK("hold: the banner ran out", !g_lock_flash.armed);

    /* Input under Hold: drained, and applied to nothing. */
    hl_scroll(2);
    hl_run_ms(800);
    CHECK("hold: the wheel under Hold moved nothing",
          g_music_sel == sel0 && top_is(SCR_MENU, SCR_MUSIC));
    tap(HL_BTN_MENU, 300);
    CHECK("hold: MENU under Hold did nothing", top_is(SCR_MENU, SCR_MUSIC));

    /* Into the dark under Hold (playing: no idle sleep), then Hold off. */
    CHECK("hold: the screen went out under Hold",
          hl_run_until(pred_dark, NULL, 500, 45000));
    hl_set_hold(0);
    hl_run_ms(300);
    CHECK("hold: switched off, the panel lights", !g_locked && lit_full());
    hl_run_ms(2500);
    CHECK("hold: ...and stays lit (the edge is an input)", lit_full());
}

/* ---- banners: the first touch ends the On-The-Go banner --------------------- */

static void otg_banner(void)
{
    CHECK("banners: the Music menu", to_music());
    wheel(MU_SONGS - g_music_sel);
    tap(HL_BTN_SELECT, 800);
    CHECK("banners: Songs", top_is(SCR_MUSIC, SCR_SONGS));
    int n0 = g_otg.n;
    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(800);
    CHECK("banners: a hold added a song, the banner is up",
          g_otg.n == n0 + 1 && g_otg_flash.armed);
    int sel0 = g_song_sel;
    hl_scroll(1);
    hl_run_ms(250);
    CHECK("banners: a wheel turn ended it, and moved the cursor",
          !g_otg_flash.armed && g_song_sel == sel0 + 1);
}

/* ---- wake ------------------------------------------------------------------ */

static void wake(void)
{
    uint32_t c0 = hl_stats().piezo_clicks;
    tap(HL_BTN_SELECT, 600);          /* Songs: plays the selected row */
    CHECK("wake: a press clicks", hl_stats().piezo_clicks > c0);
    CHECK("wake: Songs plays the row", hl_run_until(pred_playing, NULL, 50, 3000));
    CHECK("wake: dimmed", hl_run_until(pred_dim, NULL, 250, 20000));
    hl_scroll(1);
    hl_run_ms(300);
    CHECK("wake: a wheel turn relights it in full", lit_full());
}

/* ---- modal: the charging screen ------------------------------------------- */

static void modal(void)
{
    CHECK("modal: the Music menu", to_music());
    hl_set_external_power(1);
    hl_run_ms(800);
    CHECK("modal: plugged in, the charging screen", scr_cur() == SCR_CHARGING);
    tap(HL_BTN_MENU, 800);
    CHECK("modal: a press dismissed it, and did nothing else",
          top_is(SCR_MENU, SCR_MUSIC));
    hl_set_external_power(0);
    hl_run_ms(800);
    CHECK("modal: unplugged, still the Music menu", top_is(SCR_MENU, SCR_MUSIC));
}

/* ---- track end under the scrubber ----------------------------------------- */

static void track_end(void)
{
    CHECK("end: Albums, on Mono Mix", to_music() && to_album_row("Mono Mix"));
    tap(HL_BTN_PLAY, 200);            /* Mono Flac (2 s), then Mono Mp3 (2 s) */
    CHECK("end: into Mono Mp3", hl_run_until(pred_second, NULL, 50, 5000) && playing());
    tap(HL_BTN_SELECT, 200);
    CHECK("end: scrubbing on Now Playing", scr_cur() == SCR_NOWPLAYING && np_scrubbing());
    hl_run_ms(2200);                  /* the queue ends; < 4 s of quiet       */
    CHECK("end: the queue ended", !player_active());
    CHECK("end: ...and took the scrubber with it", !np_scrubbing());
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/gestures_XXXXXX";
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

    transport();
    select_np();
    hold_switch();
    otg_banner();
    wake();
    modal();
    track_end();

    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) fputs(hl_log(), stderr);
    printf("gestures: done at fake time %.1f s\n", (double)s.now_us / 1e6);
    unlink(disk);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}
