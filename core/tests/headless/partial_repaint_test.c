/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/partial_repaint_test.c — the fast repaints, judged against
 * the slow one.
 *
 * Two paths in app/paint.c draw a screen WITHOUT the ordinary full render:
 *
 *   list_repaint_partial  a selection move on a still list window repaints
 *                         only the old and new rows, the header's "n / m"
 *                         value and the scrollbar; a cover that lands on the
 *                         album list repaints only its row. Everything else on
 *                         the panel is left as the last frame drew it, so the
 *                         path is only right if what it leaves IS what a full
 *                         paint would draw — which is what its guards (same
 *                         screen, depth, list epoch, count, chrome, window; a
 *                         header title clear of the value box) are for.
 *   paint_current_screen  the screen behind the Hold banner, painted in one
 *                         go when the banner goes up, and the frame a wake
 *                         lights the backlight over (painted while dark,
 *                         after the white suspend frame). The main loop
 *                         repaints right after a wake, so that frame is read
 *                         back with hl_lit_frame(): the panel as it was when
 *                         the backlight went from off to on.
 *
 * The ORACLE is the full render itself. After every present the test stops
 * the (frozen) firmware, saves its framebuffer, runs the screen's own full
 * renderer into it (the same call run_ui's render switch makes), compares the
 * result with the PANEL — what was actually presented — and puts the
 * framebuffer back. Nothing here says what a screen should look like; only
 * that a partial repaint and a full one agree, byte for byte. The renderers
 * are the firmware's own code on the test thread while the firmware thread is
 * parked; the marquee slot they re-register is saved and restored with the
 * framebuffer, and fake time is held still across the call (hl_hold_time: a
 * renderer's mq_text reads the USEC_TIMER), so the oracle moves nothing the
 * firmware will see.
 *
 * And, separately, that the partial path was TAKEN: a move whose frame went
 * out with no full-frame present. A partial path that always declined would
 * pass every comparison above; this count is what catches that.
 *
 * Cases (each its own power-on of a fresh copy of the disk):
 *
 *   menus     the main menu and Music (nine rows: its window scrolls) down
 *             and back up, row by row
 *   library   Artists, an artist's albums ("All Songs" first), Albums, an
 *             album's tracklist, Songs (seven two-line rows: the window
 *             scrolls), Genres, a genre's songs
 *   playlists the "Added" banner over the album list and the list after it
 *             fades (a repaint with nothing moved: the partial path must
 *             decline it); Playlists, the Mixed playlist, On-The-Go with three
 *             songs held onto it, and the queue (Now Playing's list) of a
 *             paused album
 *   covers    on the variant disk (--covers --long-playlist): the album list's
 *             chips arrive after the first frame and land through g_lp.rows
 *             (one row band each, no full frame), then the moves over the
 *             covered rows; and a twelve-row playlist whose name reaches the
 *             header's value box (every move a full frame, still right)
 *   defeat    moves that must NOT be partial, still correct: Hold released
 *             in the same pass as a move (the chrome's padlock), PLAY and a
 *             move in one pass (the strip's play state), the last On-The-Go
 *             row removed under the cursor (the count; the cursor clamps), and
 *             the window scrolls already in `menus` and `library`
 *   hold      Hold engaged and released on every screen the banner is hosted
 *             on: the panel below the banner while it is up, and the whole
 *             panel after it fades, equal the full render
 *   wake      PLAY held to sleep, MENU to wake, on every list, Settings,
 *             Search, Now Playing, the queue and the battery modal: the frame
 *             the backlight came on over is the full render. (Behind the Hold
 *             banner the framebuffer already holds the same screen, so a
 *             paint_current_screen that skipped it would not show there; after
 *             the white suspend frame it does.)
 *
 * Usage: partial_repaint_test DISK.img DISK.img.ranges VARIANT.img VARIANT.img.ranges
 *                             [SHOT_DIR]
 */

#include "headless.h"
#include "hl_screen.h"
#include "../xfail.h"

#include "kernel/console.h"
#include "player/player.h"
#include "app/albums.h"
#include "app/battery_ui.h"
#include "app/lists.h"
#include "app/marquee.h"
#include "app/nav.h"
#include "app/nowplaying.h"
#include "app/otg.h"
#include "app/playlists.h"
#include "app/search_glue.h"
#include "app/settings_glue.h"
#include "app/statusbar.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "partial-repaint", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

static const char *g_shots;
static const char *g_case = "";
static int g_shot_n;

/* ---- the oracle ------------------------------------------------------------ */

#define NPX (HL_W * HL_H)
static uint16_t g_full[NPX];      /* the full render of the current state */
static uint16_t g_saved[NPX];

/* The full render of whatever is on top of the stack: run_ui's render switch
 * (and Now Playing's), into the framebuffer, copied out to g_full; the
 * framebuffer and the marquee slot are then put back exactly as they were. */
static void full_render(void)
{
    uint16_t *fb = (uint16_t *)console_framebuffer();
    marquee_t mq = g_mq;
    hl_hold_time(1);
    memcpy(g_saved, fb, sizeof g_saved);
    g_mq.active = 0;
    switch (scr_cur()) {
    case SCR_MENU:       main_menu_render();                            break;
    case SCR_MUSIC:      music_menu_render();                           break;
    case SCR_ARTISTS:    artists_render(g_artist_sel);                  break;
    case SCR_SONGS:      songs_render(g_song_sel);                      break;
    case SCR_GENRES:     genres_render(g_genre_sel);                    break;
    case SCR_PLAYLISTS:  playlists_render(g_pl_sel);                    break;
    case SCR_PLAYLIST:   playlist_render(g_plt_sel);                    break;
    case SCR_OTG:        otg_render(g_otg_sel);                         break;
    case SCR_SEARCH:     search_render_cur();                           break;
    case SCR_BROWSER:    browse_render(g_dir_depth ? g_det_sel : g_br_sel); break;
    case SCR_QUEUE:      queue_render(g_queue_sel);                     break;
    case SCR_SETTINGS:   settings_render_cur();                         break;
    case SCR_NOWPLAYING:
        nowplaying_render(player_track_name(), player_elapsed_s(),
                          player_total_s(), player_buf_pct());
        break;
    case SCR_BATTERY:    battery_modal_paint();                         break;
    case SCR_CHARGING:   charging_screen_paint();                       break;
    default:
        /* A screen this switch does not know: the framebuffer would be
         * compared with itself and every check would pass. Add its renderer
         * here (as run_ui's switch has it). */
        CHECK("oracle: a full renderer for the screen on top", 0);
        fprintf(stderr, "  oracle: no renderer for scr %d\n", (int)scr_cur());
        break;
    }
    memcpy(g_full, fb, sizeof g_full);
    memcpy(fb, g_saved, sizeof g_saved);
    g_mq = mq;
    hl_hold_time(0);
}

static void shot(const char *name)
{
    if (!g_shots) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/partial_%s_%02d_%s.bmp",
             g_shots, g_case, g_shot_n++, name);
    (void)hl_write_bmp(path);
}

/* The frame judged: the panel, or (g_judge_lit) what the backlight last came
 * on over — a wake's frame, which the main loop paints over at once. */
static int g_judge_lit;

/* Pixels of that frame in rows [y0, HL_H) that differ from the full render
 * of the current state. The first difference is described into `why`. */
static int g_compares;          /* checkpoints judged, for the summary line */

static int diff_from_full(int y0, char *why, size_t whylen)
{
    g_compares++;
    full_render();
    const uint16_t *p = g_judge_lit ? hl_lit_frame(NULL) : hl_panel();
    int n = 0, fx = -1, fy = -1, x1 = 0, y1 = 0, x0 = HL_W, yy0 = HL_H;
    for (int y = y0; y < HL_H; y++) {
        for (int x = 0; x < HL_W; x++) {
            if (p[y * HL_W + x] == g_full[y * HL_W + x]) continue;
            if (!n) { fx = x; fy = y; }
            n++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < yy0) yy0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (n && why) {
        snprintf(why, whylen, "%d px differ, first (%d,%d) panel %04x full %04x, "
                 "box x %d..%d y %d..%d", n, fx, fy, p[fy * HL_W + fx],
                 g_full[fy * HL_W + fx], x0, x1, yy0, y1);
    }
    return n;
}

/* One labelled "the panel is the full render" assertion. */
static int is_full(const char *label, int y0)
{
    char why[160] = "";
    int n = diff_from_full(y0, why, sizeof why);
    char l[200];
    snprintf(l, sizeof l, "%s: the panel is the full render%s", label,
             y0 ? " (below the banner)" : "");
    CHECK(l, n == 0);
    if (n) {
        fprintf(stderr, "  %s: scr %d depth %d: %s\n", label, (int)scr_cur(),
                g_dir_depth, why);
        shot("mismatch");
    }
    return n == 0;
}

/* ---- moves ------------------------------------------------------------------ */

typedef struct {
    int steps, wrong, partial, full;
    char first[200];
} sweep_t;

/* One detent; then let it present. A move is counted PARTIAL when its frame
 * went out with no full-frame present (rects only), FULL otherwise; a detent
 * against the end of a list presents nothing and counts as neither. */
static void step(sweep_t *s, int dir, uint32_t settle_ms)
{
    hl_stats_t a = hl_stats();
    hl_scroll(dir);
    hl_run_ms(settle_ms);
    hl_stats_t b = hl_stats();
    s->steps++;
    if (b.presents_full != a.presents_full)      s->full++;
    else if (b.presents_rect != a.presents_rect) s->partial++;
    char why[160];
    if (diff_from_full(0, why, sizeof why)) {
        if (!s->wrong) {
            snprintf(s->first, sizeof s->first, "step %d (dir %+d) scr %d: %s",
                     s->steps, dir, (int)scr_cur(), why);
            shot("sweep_mismatch");
        }
        s->wrong++;
    }
}

/* Down `n` rows one detent at a time, then back up. `want_partial`: this list
 * is one list_repaint_partial accepts, so at least one move must have taken
 * it. `want_full`: the window scrolls somewhere in the sweep, so at least one
 * move must have been a full frame. */
static void sweep(const char *label, int n, int want_partial, int want_full)
{
    sweep_t s;
    memset(&s, 0, sizeof s);
    is_full(label, 0);
    for (int i = 0; i < n; i++) step(&s, +1, 250);
    for (int i = 0; i < n; i++) step(&s, -1, 250);
    char l[200];
    snprintf(l, sizeof l, "%s: every move's frame is the full render (%d moves)",
             label, s.steps);
    CHECK(l, s.wrong == 0);
    if (s.wrong) fprintf(stderr, "  %s: %d wrong; %s\n", label, s.wrong, s.first);
    printf("%s: %d moves, %d partial, %d full\n", label, s.steps, s.partial, s.full);
    if (want_partial) {
        snprintf(l, sizeof l, "%s: the partial path was taken", label);
        CHECK(l, s.partial > 0);
    }
    if (want_full) {
        snprintf(l, sizeof l, "%s: the window scrolled (a full frame) too", label);
        CHECK(l, s.full > 0);
    }
}

/* ---- navigation (as search_walk_test.c's) ------------------------------------ */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int header(const char *s) { return hl_find_text_in(hl_panel(), s, 0, 40, NULL); }
static int in_list(const char *s) { return hl_find_text_in(hl_panel(), s, 40, HL_H, NULL); }
static int selected(const char *s)
{
    hl_text_hit_t h;
    return hl_find_text_in(hl_panel(), s, 40, HL_H, &h) && lum(h.ink) > lum(h.bg);
}
static void tap(unsigned btn) { hl_tap(btn, 500); }
static int pred_header(void *a) { return header((const char *)a); }

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

/* Back to the main menu, then down the named rows, SELECTing each. */
static void go(const char *const *path, int n)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    for (int i = 0; i < n; i++) {
        char l[120];
        snprintf(l, sizeof l, "nav: %s", path[i]);
        /* "Songs" is also the tail of "Shuffle Songs" below it, which the
         * text search can find first: go to Albums and one row down. */
        int ok;
        if (strcmp(path[i], "Songs") == 0) {
            ok = select_row("Albums");
            hl_scroll(1);
            hl_run_ms(400);
        } else {
            ok = select_row(path[i]);
        }
        CHECK(l, ok);
        if (!ok) shot("nav");
        tap(HL_BTN_SELECT);
        hl_run_ms(1000);              /* a list read on entry; maybe a spin-up */
    }
}
#define GO(...) do { const char *const p_[] = { __VA_ARGS__ }; \
                     go(p_, (int)(sizeof p_ / sizeof p_[0])); } while (0)

/* Hold Select past SEL_HOLD_US (450 ms): On-The-Go add / remove, the queue. */
static void hold_select(void)
{
    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(800);
}

/* ---- disk + session -------------------------------------------------------- */

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
    if (T.fails && getenv("HL_DUMP_LOG")) fputs(hl_log(), stderr);
    printf("%s: fake time %.1f s, %u presents (%u full), %d oracle compares\n",
           g_case, (double)s.now_us / 1e6, s.presents_full + s.presents_rect,
           s.presents_full, g_compares);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);                     /* the firmware thread is frozen mid-wait */
}

/* ---- cases ----------------------------------------------------------------- */

static void case_menus(void)
{
    CHECK("menus: the main menu", header("Core"));
    sweep("main menu", 4, 1, 0);
    GO("Music");
    CHECK("menus: Music", header("Music"));
    sweep("music", 8, 1, 1);
}

static void case_library(void)
{
    GO("Music", "Artists");
    CHECK("library: Artists", header("Artists"));
    sweep("artists", 3, 1, 0);
    CHECK("library: an artist", select_row("Test Tones"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    CHECK("library: the artist's albums, All Songs first", selected("All Songs"));
    sweep("artist albums", 2, 1, 0);

    GO("Music", "Albums");
    CHECK("library: Albums", header("Albums"));
    sweep("albums", 3, 1, 0);
    CHECK("library: an album", select_row("Beethoven Bits"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    CHECK("library: the tracklist", in_list("Allegro"));
    sweep("tracklist", 3, 1, 0);

    GO("Music", "Songs");
    CHECK("library: Songs", header("Songs"));
    sweep("songs", 7, 1, 1);

    GO("Music", "Genres");
    CHECK("library: Genres", header("Genres"));
    sweep("genres", 1, 0, 0);       /* one genre: nothing moves */
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    sweep("genre songs", 7, 1, 1);
}

static void case_playlists(void)
{
    /* Three songs onto On-The-Go first: an album row held. */
    GO("Music", "Albums");
    CHECK("playlists: the album row", select_row("Beethoven Bits"));
    hold_select();
    is_full("playlists: Added banner up", top_banner_h());
    hl_run_ms(1500);
    /* The fade is a repaint with nothing moved and nothing in g_lp.rows: the
     * partial path must decline it, or the banner stays. */
    is_full("playlists: the Added banner faded", 0);

    GO("Music", "Playlists");
    CHECK("playlists: Playlists", hl_run_until(pred_header, "Playlists", 250, 5000));
    sweep("playlists", 2, 1, 0);
    CHECK("playlists: Mixed", select_row("Mixed"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    sweep("playlist", 6, 1, 0);

    tap(HL_BTN_MENU);
    CHECK("playlists: On-The-Go", select_row("On-The-Go"));
    tap(HL_BTN_SELECT);
    CHECK("playlists: the On-The-Go screen",
          hl_run_until(pred_header, "On-The-Go", 250, 5000));
    sweep("on-the-go", 5, 1, 0);

    /* The queue: an album played, paused (its bars stand still), then
     * Now Playing's list (Select held). */
    GO("Music", "Albums");
    CHECK("playlists: the album to play", select_row("Beethoven Bits"));
    tap(HL_BTN_PLAY);
    hl_run_ms(1500);
    tap(HL_BTN_PLAY);                /* pause */
    hl_run_ms(500);
    CHECK("playlists: paused", !hl_stats().audio_running);
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    CHECK("playlists: Now Playing row", select_row("Now Playing"));
    tap(HL_BTN_SELECT);
    hl_run_ms(800);
    hold_select();
    hl_run_ms(500);
    CHECK("playlists: the queue", header("Now Playing") && in_list("Allegro"));
    sweep("queue", 3, 1, 0);
}

static void case_covers(void)
{
    GO("Music");
    CHECK("covers: Music > Albums", select_row("Albums"));
    /* The first frame goes out with placeholder chips; the pump lands the
     * covers after it. Each landed cover repaints its row through g_lp.rows,
     * not the whole panel. */
    hl_press(HL_BTN_SELECT, 60);
    hl_stats_t a0 = hl_stats();
    for (int i = 0; i < 100 && hl_stats().presents_full == a0.presents_full; i++) {
        hl_run_ms(1);
    }
    CHECK("covers: Albums", header("Albums"));
    shot("covers_first");
    uint32_t h0 = hl_frame_hash(hl_panel());
    hl_stats_t a = hl_stats();
    hl_run_ms(2000);
    shot("covers_landed");
    hl_stats_t b = hl_stats();
    CHECK("covers: the covers landed after the first frame",
          hl_frame_hash(hl_panel()) != h0);
    CHECK("covers: ...as row repaints, not full frames",
          b.presents_full == a.presents_full && b.presents_rect > a.presents_rect);
    is_full("covers: after the covers landed", 0);
    sweep("covers", 3, 1, 0);
    /* Back in from a tracklist: the covers are resident, the list repaints. */
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    tap(HL_BTN_MENU);
    hl_run_ms(2000);
    sweep("covers again", 3, 1, 0);

    /* A header title that reaches the "n / m" value box: the box's clear
     * would eat its tail, and the title's ellipsis moves when the value
     * widens ("9 / 12" -> "10 / 12"), so every move here is a full frame. */
    GO("Music", "Playlists");
    CHECK("long title: Playlists", hl_run_until(pred_header, "Playlists", 250, 5000));
    /* Its row is ellipsised: it sorts after Mixed. */
    CHECK("long title: Mixed", select_row("Mixed"));
    hl_scroll(1);
    hl_run_ms(400);
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    CHECK("long title: the twelve-row playlist", header("1 / 12"));
    sweep("long title", 11, 0, 1);
}

/* A move in the same pass as something that changes what the partial path
 * would leave behind. */
static void case_defeat(void)
{
    /* Hold: the padlock is in the status strip (chrome_key's g_locked). Lock,
     * let the banner fade (the list repaints, locked); then release Hold AND
     * turn the wheel at the same instant. The input drain drops the unlock
     * banner on that first event, so the move is rendered by the ordinary
     * path — with the padlock gone. */
    GO("Music", "Artists");
    hl_set_hold(1);
    hl_run_ms(2500);
    is_full("defeat: locked, the banner faded", 0);
    hl_set_hold(0);
    hl_scroll(1);
    hl_run_ms(250);
    is_full("defeat: Hold released in the move's pass", 0);
    hl_set_hold(1);
    hl_run_ms(2500);
    hl_set_hold(0);
    hl_scroll(-1);
    hl_run_ms(250);
    is_full("defeat: Hold released in the move's pass (back up)", 0);

    /* PLAY and a move in one pass: the strip's play/pause state. */
    GO("Music", "Albums");
    CHECK("defeat: an album to play", select_row("Long Player"));
    tap(HL_BTN_PLAY);
    hl_run_ms(1500);
    tap(HL_BTN_PLAY);                       /* paused */
    hl_run_ms(500);
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    hl_run_ms(500);
    is_full("defeat: main menu, paused", 0);
    hl_press(HL_BTN_PLAY, 60);              /* resume ... */
    hl_scroll(1);                           /* ... and move, same pass */
    hl_run_ms(300);
    is_full("defeat: PLAY in the move's pass", 0);
    hl_press(HL_BTN_PLAY, 60);              /* pause again, moving back */
    hl_scroll(-1);
    hl_run_ms(300);
    is_full("defeat: PLAY in the move's pass (pause)", 0);

    /* The count: three songs on On-The-Go, the cursor on the last; removing
     * it clamps the cursor onto the row above (a move) as the list shrinks. */
    GO("Music", "Albums");
    CHECK("defeat: the album row", select_row("Beethoven Bits"));
    hold_select();
    hl_run_ms(1500);
    GO("Music", "Playlists");
    CHECK("defeat: On-The-Go", select_row("On-The-Go"));
    tap(HL_BTN_SELECT);
    CHECK("defeat: the On-The-Go screen",
          hl_run_until(pred_header, "On-The-Go", 250, 5000));
    CHECK("defeat: the last song", select_row("Sine"));
    is_full("defeat: On-The-Go, the last row selected", 0);
    hold_select();                          /* remove Sine */
    hl_run_ms(1500);
    CHECK("defeat: Sine removed", !in_list("Sine"));
    is_full("defeat: the last row removed under the cursor", 0);
    hold_select();                          /* and Andante */
    hl_run_ms(1500);
    is_full("defeat: the next last row removed", 0);
}

/* Hold on and off: the banner over the real screen, then the real screen. */
static void hold_check(const char *label)
{
    char l[160];
    snprintf(l, sizeof l, "%s: before Hold", label);
    is_full(l, 0);
    hl_set_hold(1);
    hl_run_ms(300);
    snprintf(l, sizeof l, "%s: Hold banner up", label);
    is_full(l, top_banner_h());
    hl_run_ms(2000);
    snprintf(l, sizeof l, "%s: Hold banner faded", label);
    is_full(l, 0);
    hl_set_hold(0);
    hl_run_ms(300);
    snprintf(l, sizeof l, "%s: unlock banner up", label);
    is_full(l, top_banner_h());
    hl_run_ms(2000);
    snprintf(l, sizeof l, "%s: unlock banner faded", label);
    is_full(l, 0);
}

static void case_hold(void)
{
    /* Moved off row 0 everywhere, so a paint with the wrong selection shows. */
    hl_scroll(1); hl_run_ms(300);
    hold_check("hold main menu");
    GO("Music");
    hl_scroll(2); hl_run_ms(500);
    hold_check("hold music");
    GO("Music", "Artists");
    hl_scroll(1); hl_run_ms(300);
    hold_check("hold artists");
    GO("Music", "Songs");
    hl_scroll(2); hl_run_ms(500);
    hold_check("hold songs");
    GO("Music", "Genres");
    hold_check("hold genres");
    GO("Music", "Albums");
    hl_scroll(1); hl_run_ms(300);
    hold_check("hold albums");
    CHECK("hold: an album", select_row("Mono Mix"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    hl_scroll(1); hl_run_ms(300);   /* g_det_sel 1, g_br_sel 2 */
    hold_check("hold tracklist");
    GO("Music", "Playlists");
    hl_scroll(1); hl_run_ms(300);
    hold_check("hold playlists");
    tap(HL_BTN_SELECT);              /* Mixed */
    hl_run_ms(1500);
    hl_scroll(2); hl_run_ms(500);
    hold_check("hold playlist");
    GO("Music", "Playlists", "On-The-Go");
    hold_check("hold on-the-go");
    GO("Settings");
    hl_scroll(1); hl_run_ms(300);
    hold_check("hold settings");
    GO("Music", "Search");
    hold_check("hold search");

    /* Playing something, paused: Now Playing and its queue. */
    GO("Music", "Albums");
    CHECK("hold: an album to play", select_row("Beethoven Bits"));
    tap(HL_BTN_PLAY);
    hl_run_ms(1500);
    tap(HL_BTN_PLAY);
    hl_run_ms(500);
    hold_check("hold now playing");
    hold_select();
    hl_run_ms(500);
    hl_scroll(1); hl_run_ms(300);
    hold_check("hold queue");
}

/* PLAY held past PLAY_HOLD_US (2 s): suspend. A short MENU wakes it (and is
 * swallowed). The frame the backlight came on over is the one
 * paint_current_screen rendered while the panel was still dark. */
static void wake_check(const char *label)
{
    char l[160];
    uint32_t n0, n1;
    (void)hl_lit_frame(&n0);
    hl_press(HL_BTN_PLAY, 2600);
    hl_run_ms(4000);
    snprintf(l, sizeof l, "%s: PLAY held, asleep", label);
    CHECK(l, hl_stats().backlight == 0);
    hl_press(HL_BTN_MENU, 300);
    hl_run_ms(3000);
    (void)hl_lit_frame(&n1);
    snprintf(l, sizeof l, "%s: woke, lit", label);
    CHECK(l, n1 > n0 && hl_stats().backlight > 0);
    printf("%s: woke on scr %d\n", label, (int)scr_cur());
    snprintf(l, sizeof l, "%s: the frame lit on wake", label);
    g_judge_lit = 1;
    is_full(l, 0);
    g_judge_lit = 0;
    snprintf(l, sizeof l, "%s: after the wake", label);
    is_full(l, 0);
}

static void case_wake(void)
{
    /* On-The-Go first gets three songs, so its screen is a list. */
    GO("Music", "Albums");
    CHECK("wake: the album row", select_row("Beethoven Bits"));
    hold_select();
    hl_run_ms(1500);

    hl_scroll(1); hl_run_ms(300);
    wake_check("wake albums");
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    hl_scroll(1); hl_run_ms(300);
    wake_check("wake main menu");
    GO("Music");
    hl_scroll(2); hl_run_ms(500);
    wake_check("wake music");
    GO("Music", "Artists");
    hl_scroll(1); hl_run_ms(300);
    wake_check("wake artists");
    GO("Music", "Songs");
    hl_scroll(2); hl_run_ms(500);
    wake_check("wake songs");
    GO("Music", "Genres");
    wake_check("wake genres");
    GO("Music", "Playlists");
    hl_scroll(1); hl_run_ms(300);
    wake_check("wake playlists");
    tap(HL_BTN_SELECT);              /* Mixed */
    hl_run_ms(1500);
    hl_scroll(2); hl_run_ms(500);
    wake_check("wake playlist");
    GO("Music", "Playlists", "On-The-Go");
    hl_scroll(2); hl_run_ms(500);
    wake_check("wake on-the-go");
    GO("Music", "Albums");
    CHECK("wake: an album", select_row("Mono Mix"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    hl_scroll(1); hl_run_ms(300);
    wake_check("wake tracklist");
    GO("Settings");
    hl_scroll(2); hl_run_ms(500);
    wake_check("wake settings");
    GO("Music", "Search");
    wake_check("wake search");
    GO("Music", "Albums");
    CHECK("wake: an album to play", select_row("Beethoven Bits"));
    tap(HL_BTN_PLAY);
    hl_run_ms(1500);
    tap(HL_BTN_PLAY);                /* paused: the clock stands still */
    hl_run_ms(500);
    wake_check("wake now playing");

    hold_select();                   /* Now Playing's list */
    hl_run_ms(500);
    hl_scroll(1); hl_run_ms(300);
    wake_check("wake queue");

    /* The battery modal: the cell below the disk-safe line. (The charging
     * screen is out of reach here: the PLAY that would sleep dismisses it,
     * the idle sleep does not run on external power, and no banner is
     * hosted on a modal.) */
    hl_set_battery_mv(3450);
    hl_run_ms(40000);
    CHECK("wake: the battery modal is up", scr_cur() == SCR_BATTERY);
    wake_check("wake battery");
    CHECK("wake: ...and still up after the wake", scr_cur() == SCR_BATTERY);
}

typedef struct {
    const char *name;
    int variant;                   /* boots the --covers --long-playlist disk */
    void (*run)(void);
} case_t;

static const case_t CASES[] = {
    { "menus",     0, case_menus },
    { "library",   0, case_library },
    { "playlists", 0, case_playlists },
    { "covers",    1, case_covers },
    { "defeat",    0, case_defeat },
    { "hold",      0, case_hold },
    { "wake",      0, case_wake },
};

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges VARIANT.img "
                "VARIANT.img.ranges [SHOT_DIR]\n", argv[0]);
        return 2;
    }
    g_shots = argc > 5 ? argv[5] : NULL;
    const char *only = getenv("PARTIAL_CASE");
    int ok = 1;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (only && strcmp(only, CASES[i].name) != 0) continue;
        char disk[] = "/tmp/partial_repaint_XXXXXX";
        int fd = mkstemp(disk);
        if (fd < 0 || copy_disk(CASES[i].variant ? argv[3] : argv[1], disk) != 0) {
            fprintf(stderr, "cannot copy the disk\n");
            return 2;
        }
        close(fd);
        fflush(NULL);
        pid_t p = fork();
        if (p == 0) {
            g_case = CASES[i].name;
            T.suite = CASES[i].name;
            CHECK("image: the pre-allocated files' extents are known",
                  load_ranges(CASES[i].variant ? argv[4] : argv[2]) >= 3);
            if (hl_boot(disk, 12000) != 0) {
                fprintf(stderr, "hl_boot failed\n");
                _exit(2);
            }
            CASES[i].run();
            session_end();
        }
        int st = 0;
        waitpid(p, &st, 0);
        unlink(disk);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fprintf(stderr, "partial-repaint: %s FAILED\n", CASES[i].name);
            ok = 0;
        }
    }
    return ok ? 0 : 1;
}
