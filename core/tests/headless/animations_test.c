/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/animations_test.c — the main loop's animators, while a track
 * PLAYS, judged frame by frame against the full render.
 *
 * Between full repaints run_ui pushes three animations as small rect presents
 * of their own:
 *
 *   the marquee      g_mq, the one scrolling-text slot (the Now Playing title,
 *                    or the selected row of a list when its title overflows):
 *                    redrawn at the live phase offset and its band presented,
 *                    every 33 ms (~30 fps)
 *   the queue bars   the playing row's three bars on the queue (Now Playing's
 *                    list), every 110 ms, only while playing
 *   the detail bars  the same on the album detail's tracklist
 *
 * Nothing else exercised them with music actually playing (every test that
 * opens the queue pauses first), and partial-repaint ran the marquee step
 * without ever comparing what it drew: its band could be painted any colour,
 * never presented, or drawn at the wrong phase, and the suite passed.
 *
 * THE ORACLE (as partial_repaint_test.c's): the screen's own full renderer,
 * run into the framebuffer and copied out, the framebuffer, the marquee slot
 * and the damage rect then put back. Here it runs from hl_on_present_rect, on
 * the firmware thread, INSIDE each rect present, with fake time held: so the
 * marquee offset and the bars' clock it renders are those of the instant the
 * animator drew (the step reads the timer, draws, presents — no read in
 * between that the oracle's held read would not repeat). Every animated frame
 * must equal the full render of that instant, byte for byte.
 *
 * And, separately from the oracle (which shares the firmware's constants),
 * the RATE, from the code's own numbers re-stated here:
 *
 *   marquee  presents >= 33 ms apart, the median within a few ms of it; the
 *            offset the panel shows (measured: the band matched against the
 *            whole title drawn into a private buffer) is 0 for the 1.5 s
 *            start dwell, one px per 26 ms after, the tail (tw - w) for
 *            0.9 s, then 0 again — the phase from g_mq.t0
 *   bars     presents >= 110 ms apart, the median close to it; the bars'
 *            pixels change from one step to the next; on pause the steps
 *            stop and the bars stand still (and the still frame is the full
 *            render too)
 *
 * Cases (each its own power-on of the --long-title disk: the stock library
 * plus an album "Marquee" whose one track's title overflows both the Now
 * Playing title box and a Songs row):
 *
 *   np      the long track played; Now Playing's title scrolls
 *   list    Long One played; on Songs the long row selected scrolls
 *   queue   Long One played; the queue's bars, then paused
 *   detail  Long One played; Long Player's tracklist's bars, then paused
 *
 * Usage: animations_test DISK.img DISK.img.ranges [SHOT_DIR]
 */

#include "headless.h"
#include "hl_screen.h"
#include "../xfail.h"

#include "kernel/console.h"
#include "player/player.h"
#include "ui/text.h"
#include "app/albums.h"
#include "app/layout.h"
#include "app/lists.h"
#include "app/marquee.h"
#include "app/nav.h"
#include "app/nowplaying.h"
#include "app/otg.h"
#include "app/playlists.h"
#include "app/search_glue.h"
#include "app/settings_glue.h"
#include "app/statusbar.h"
#include "app/battery_ui.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "animations", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

static const char *g_shots;
static const char *g_case = "";
static int g_shot_n;

/* The code's numbers, re-stated (app/marquee.c, kernel/main.c run_ui). */
#define MQ_STEP_US     33000u      /* the marquee step's pace                */
#define MQ_US_PER_PX   26000u
#define MQ_HOLD_START 1500000u
#define MQ_HOLD_END    900000u
#define BARS_STEP_US  110000u      /* both bars steps' pace                  */
#define PACE_SLACK_US   4000u      /* how far the median may trail the pace  */

/* ---- the oracle ------------------------------------------------------------ */

#define NPX (HL_W * HL_H)
static uint16_t g_full[NPX];
static uint16_t g_saved[NPX];

static void full_render(void)
{
    uint16_t *fb = (uint16_t *)console_framebuffer();
    marquee_t mq = g_mq;
    int dx = 0, dy = 0, dw = 0, dh = 0;
    int dmg = console_damage_get(&dx, &dy, &dw, &dh);
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
        memset(g_full, 0, sizeof g_full);   /* never equal to a real frame */
        fprintf(stderr, "  oracle: no renderer for scr %d\n", (int)scr_cur());
        memcpy(fb, g_saved, sizeof g_saved);
        g_mq = mq;
        hl_hold_time(0);
        return;
    }
    memcpy(g_full, fb, sizeof g_full);
    memcpy(fb, g_saved, sizeof g_saved);
    g_mq = mq;
    console_damage_reset();
    if (dmg) console_damage_add(dx, dy, dw, dh);
    hl_hold_time(0);
}

/* Pixels of the panel in [rx, rx+rw) x [ry, ry+rh) that differ from the full
 * render of this instant; the first difference described into `why`. */
static int diff_from_full(int rx, int ry, int rw, int rh, char *why, size_t whylen)
{
    full_render();
    const uint16_t *p = hl_panel();
    int n = 0, fx = -1, fy = -1, x0 = HL_W, x1 = 0, y0 = HL_H, y1 = 0;
    for (int y = ry; y < ry + rh; y++) {
        for (int x = rx; x < rx + rw; x++) {
            if (p[y * HL_W + x] == g_full[y * HL_W + x]) continue;
            if (!n) { fx = x; fy = y; }
            n++;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (n && why) {
        snprintf(why, whylen, "%d px differ, first (%d,%d) panel %04x full %04x, "
                 "box x %d..%d y %d..%d", n, fx, fy, p[fy * HL_W + fx],
                 g_full[fy * HL_W + fx], x0, x1, y0, y1);
    }
    return n;
}

static void shot(const char *name)
{
    if (!g_shots) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/anim_%s_%02d_%s.bmp",
             g_shots, g_case, g_shot_n++, name);
    (void)hl_write_bmp(path);
}

static int is_full(const char *label)
{
    char why[200] = "";
    int n = diff_from_full(0, 0, HL_W, HL_H, why, sizeof why);
    char l[200];
    snprintf(l, sizeof l, "%s: the panel is the full render", label);
    CHECK(l, n == 0);
    if (n) {
        fprintf(stderr, "  %s: scr %d: %s\n", label, (int)scr_cur(), why);
        shot("mismatch");
    }
    return n == 0;
}

/* ---- what the rect hook records ------------------------------------------- */

#define MAXF 2048

typedef struct {
    /* every rect present */
    int      rects, wrong;
    char     first[240];
    /* the marquee's (its band: x == g_mq.x, w == g_mq.w) */
    int      mq_n;
    uint64_t mq_t[MAXF];
    int      mq_off[MAXF], mq_want[MAXF];
    int      mq_max;                  /* tw - w */
    int      mq_bad, mq_bad_first;    /* measured != the code's formula */
    /* the bars' (w == NP_BARS_W + 1, h == NP_BARS_H, at bx/by) */
    int      bx, by;
    int      bars_n, bars_changed;
    uint64_t bars_t[MAXF];
    uint16_t bars_px[(NP_BARS_W + 1) * NP_BARS_H];
} watch_t;

static watch_t W;

/* The whole marquee title drawn at offset 0 into a private buffer as wide as
 * it, as draw_marquee draws it into the panel (bg, then the glyphs, clipped to
 * [cy0, cy1)). The panel's band at offset `off` is columns [off, off + w). */
static uint16_t *g_ref;
static int       g_ref_w, g_ref_top, g_ref_bot;
static char      g_ref_text[MQ_TEXT_MAX + 1];

static void ref_build(void)
{
    if (g_ref && strcmp(g_ref_text, g_mq.text) == 0 && g_ref_w == g_mq.tw + 1)
        return;
    free(g_ref);
    g_ref_w = g_mq.tw + 1;
    g_ref = malloc((size_t)g_ref_w * HL_H * sizeof *g_ref);
    if (!g_ref) { fprintf(stderr, "out of memory\n"); _exit(2); }
    for (int i = 0; i < g_ref_w * HL_H; i++) g_ref[i] = g_mq.bg;
    text_draw_clip_v(g_ref, g_ref_w, HL_H, 0, g_mq.y, g_mq.text, g_mq.font,
                     g_mq.ink, 0, g_ref_w, g_mq.cy0, g_mq.cy1);
    snprintf(g_ref_text, sizeof g_ref_text, "%s", g_mq.text);
    g_ref_top = g_mq.y - text_ascent(g_mq.font);
    g_ref_bot = g_mq.y + text_descent(g_mq.font);
    if (g_ref_top < g_mq.cy0) g_ref_top = g_mq.cy0;
    if (g_ref_bot > g_mq.cy1) g_ref_bot = g_mq.cy1;
}

/* The scroll offset the panel shows in the marquee band: the one offset whose
 * columns of the reference are the band; -1 none, -2 more than one. */
static int mq_measure(void)
{
    ref_build();
    const uint16_t *p = hl_panel();
    int found = -1;
    for (int off = 0; off <= g_mq.tw - g_mq.w; off++) {
        int ok = 1;
        for (int y = g_ref_top; ok && y < g_ref_bot; y++) {
            const uint16_t *pr = &p[y * HL_W + g_mq.x];
            const uint16_t *rr = &g_ref[y * g_ref_w + off];
            if (memcmp(pr, rr, (size_t)g_mq.w * sizeof *pr) != 0) ok = 0;
        }
        if (!ok) continue;
        if (found >= 0) return -2;
        found = off;
    }
    return found;
}

/* The code's offset for phase time t_us (app/marquee.c mq_offset). */
static int mq_want(int tw, int w, uint32_t t_us)
{
    if (tw <= w) return 0;
    uint32_t max_off = (uint32_t)(tw - w);
    uint32_t cycle = MQ_HOLD_START + max_off * MQ_US_PER_PX + MQ_HOLD_END;
    uint32_t tp = t_us % cycle;
    if (tp < MQ_HOLD_START) return 0;
    uint32_t off = (tp - MQ_HOLD_START) / MQ_US_PER_PX;
    return (int)(off > max_off ? max_off : off);
}

/* Every rect present, inside it, with time held at its instant. An
 * animator's own present (the marquee band, the bars box) is judged on the
 * WHOLE panel: everything else on it must still be the full render of this
 * instant. Any other present (Now Playing's transport band once a second) is
 * judged inside its rect only: the marquee band beside it was drawn by the
 * last marquee step, up to 33 ms ago, and the full render of this instant
 * scrolls it on. */
static void on_rect(void *user, int x, int y, int w, int h)
{
    (void)user;
    uint64_t now = hl_stats().now_us;
    int is_mq   = g_mq.active && x == g_mq.x && w == g_mq.w;
    int is_bars = x == W.bx && y == W.by && w == NP_BARS_W + 1 && h == NP_BARS_H;
    W.rects++;
    char why[200];
    int n = (is_mq || is_bars) ? diff_from_full(0, 0, HL_W, HL_H, why, sizeof why)
                               : diff_from_full(x, y, w, h, why, sizeof why);
    if (n) {
        if (!W.wrong) {
            snprintf(W.first, sizeof W.first, "rect %d,%d %dx%d at %.3f s: %s",
                     x, y, w, h, (double)now / 1e6, why);
            shot("rect_mismatch");
        }
        W.wrong++;
    }
    if (is_mq && W.mq_n < MAXF) {
        int i = W.mq_n++;
        W.mq_t[i]    = now;
        W.mq_max     = g_mq.tw - g_mq.w;
        W.mq_off[i]  = mq_measure();
        W.mq_want[i] = mq_want(g_mq.tw, g_mq.w, (uint32_t)now - g_mq.t0);
        if (W.mq_off[i] != W.mq_want[i] && !W.mq_bad++) W.mq_bad_first = i;
    }
    if (is_bars && W.bars_n < MAXF) {
        uint16_t px[(NP_BARS_W + 1) * NP_BARS_H];
        hl_copy_rect(px, hl_panel(), x, y, w, h);
        if (W.bars_n && memcmp(px, W.bars_px, sizeof px) != 0) W.bars_changed++;
        memcpy(W.bars_px, px, sizeof px);
        W.bars_t[W.bars_n++] = now;
    }
}

static void watch(int bx, int by)
{
    memset(&W, 0, sizeof W);
    W.bx = bx;
    W.by = by;
    hl_on_present_rect(on_rect, NULL);
}

static void unwatch(void) { hl_on_present_rect(NULL, NULL); }

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* The gaps between consecutive presents: the smallest, and the median. */
static void gaps(const uint64_t *t, int n, uint64_t *min, uint64_t *med)
{
    static uint64_t g[MAXF];
    *min = *med = 0;
    if (n < 2) return;
    for (int i = 1; i < n; i++) g[i - 1] = t[i] - t[i - 1];
    qsort(g, (size_t)(n - 1), sizeof g[0], cmp_u64);
    *min = g[0];
    *med = g[(n - 1) / 2];
}

/* Every rect present while watching was the full render of its instant. */
static void check_frames(const char *label)
{
    char l[200];
    snprintf(l, sizeof l, "%s: every animated frame is the full render (%d rects)",
             label, W.rects);
    CHECK(l, W.rects > 0 && W.wrong == 0);
    if (W.wrong) fprintf(stderr, "  %s: %d wrong; first %s\n", label, W.wrong, W.first);
}

/* ---- the marquee, over `ms` of playing ------------------------------------- */

static void marquee_run(const char *label, uint32_t ms)
{
    CHECK("marquee: a target is registered", g_mq.active);
    if (!g_mq.active) { shot("no_marquee"); return; }
    printf("%s: marquee \"%s\" tw %d in w %d at (%d,%d)\n", label, g_mq.text,
           g_mq.tw, g_mq.w, g_mq.x, g_mq.y);
    watch(-1, -1);
    hl_run_ms(ms);
    unwatch();
    char l[200];
    check_frames(label);

    uint64_t gmin, gmed;
    gaps(W.mq_t, W.mq_n, &gmin, &gmed);
    printf("%s: %d marquee presents, gap min %llu med %llu us, max offset %d\n",
           label, W.mq_n, (unsigned long long)gmin, (unsigned long long)gmed,
           W.mq_max);
    snprintf(l, sizeof l, "%s: the marquee band was presented, ~30 fps", label);
    CHECK(l, W.mq_n >= (int)(ms * 1000u / (MQ_STEP_US + PACE_SLACK_US)));
    snprintf(l, sizeof l, "%s: ...never sooner than 33 ms apart", label);
    CHECK(l, W.mq_n > 1 && gmin >= MQ_STEP_US);
    snprintf(l, sizeof l, "%s: ...the median gap the 33 ms step", label);
    CHECK(l, gmed >= MQ_STEP_US && gmed <= MQ_STEP_US + PACE_SLACK_US);

    snprintf(l, sizeof l, "%s: the offset shown is the code's rate and phase "
             "(0 for 1.5 s, 1 px / 26 ms, the tail 0.9 s)", label);
    CHECK(l, W.mq_n > 0 && W.mq_bad == 0);
    if (W.mq_bad) {
        int i = W.mq_bad_first;
        fprintf(stderr, "  %s: %d of %d frames off; first at %.3f s (phase %.3f s) "
                "shows %d, want %d\n", label, W.mq_bad, W.mq_n,
                (double)W.mq_t[i] / 1e6,
                (double)(uint32_t)((uint32_t)W.mq_t[i] - g_mq.t0) / 1e6,
                W.mq_off[i], W.mq_want[i]);
    }
    /* The shape over the run: from the start dwell, up the scroll, a dwell on
     * the tail, and back to the start (the wrap) — at least once each. */
    int start = 0, mid = 0, tail = 0, wraps = 0, back = 0;
    for (int i = 0; i < W.mq_n; i++) {
        int o = W.mq_off[i];
        if (o == 0) start++;
        else if (o == W.mq_max) tail++;
        else if (o > 0) mid++;
        if (i && W.mq_off[i - 1] == W.mq_max && o == 0) wraps++;
        if (i && o >= 0 && W.mq_off[i - 1] > o && o != 0) back++;
    }
    printf("%s: %d frames at the start, %d scrolling, %d on the tail, %d wraps\n",
           label, start, mid, tail, wraps);
    snprintf(l, sizeof l, "%s: dwells, scrolls, dwells on the tail and wraps",
             label);
    CHECK(l, start > 0 && mid > 0 && tail > 0 && wraps > 0 && back == 0);
    /* The dwells' lengths, in frames at the pace (to a frame either way). */
    int run0 = 0, runt = 0, best0 = 0, bestt = 0;
    for (int i = 0; i < W.mq_n; i++) {
        run0 = W.mq_off[i] == 0 ? run0 + 1 : 0;
        runt = W.mq_off[i] == W.mq_max ? runt + 1 : 0;
        if (run0 > best0) best0 = run0;
        if (runt > bestt) bestt = runt;
    }
    int want0 = (int)(MQ_HOLD_START / MQ_STEP_US), wantt = (int)(MQ_HOLD_END / MQ_STEP_US);
    snprintf(l, sizeof l, "%s: the start dwell ~1.5 s (%d frames), the tail ~0.9 s "
             "(%d frames)", label, best0, bestt);
    CHECK(l, best0 >= want0 - 2 && best0 <= want0 + 2
             && bestt >= wantt - 2 && bestt <= wantt + 2);
}

/* ---- the bars --------------------------------------------------------------- */

/* `pause` pauses the player and leaves the same screen on top. */
static void bars_run(const char *label, int bx, int by, void (*pause)(void))
{
    char l[200];
    CHECK("bars: playing", hl_stats().audio_running && !player_paused());
    watch(bx, by);
    hl_run_ms(3000);
    unwatch();
    check_frames(label);
    uint64_t gmin, gmed;
    gaps(W.bars_t, W.bars_n, &gmin, &gmed);
    printf("%s: %d bars presents at (%d,%d), %d changed, gap min %llu med %llu us\n",
           label, W.bars_n, bx, by, W.bars_changed, (unsigned long long)gmin,
           (unsigned long long)gmed);
    snprintf(l, sizeof l, "%s: the bars animate while playing", label);
    CHECK(l, W.bars_n >= (int)(3000000u / (BARS_STEP_US + PACE_SLACK_US)));
    snprintf(l, sizeof l, "%s: ...their pixels change step to step", label);
    CHECK(l, W.bars_n > 1 && W.bars_changed * 10 >= (W.bars_n - 1) * 9);
    snprintf(l, sizeof l, "%s: ...never sooner than 110 ms apart", label);
    CHECK(l, W.bars_n > 1 && gmin >= BARS_STEP_US);
    snprintf(l, sizeof l, "%s: ...the median gap the 110 ms step", label);
    CHECK(l, gmed >= BARS_STEP_US && gmed <= BARS_STEP_US + PACE_SLACK_US);

    /* Paused: the step stops and the bars stand still. */
    int scr = (int)scr_cur();
    pause();
    CHECK("bars: paused, the same screen", player_paused() && (int)scr_cur() == scr);
    uint16_t still[(NP_BARS_W + 1) * NP_BARS_H], now[sizeof still / sizeof still[0]];
    hl_copy_rect(still, hl_panel(), bx, by, NP_BARS_W + 1, NP_BARS_H);
    snprintf(l, sizeof l, "%s: paused", label);
    is_full(l);
    watch(bx, by);
    hl_run_ms(2000);
    unwatch();
    hl_copy_rect(now, hl_panel(), bx, by, NP_BARS_W + 1, NP_BARS_H);
    snprintf(l, sizeof l, "%s: paused, no bars step for 2 s (%d)", label, W.bars_n);
    CHECK(l, W.bars_n == 0);
    snprintf(l, sizeof l, "%s: paused, the bars stand still", label);
    CHECK(l, memcmp(still, now, sizeof still) == 0);
    snprintf(l, sizeof l, "%s: paused, 2 s on", label);
    is_full(l);
}

/* ---- navigation (as partial_repaint_test.c's) ------------------------------ */

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

static void go(const char *const *path, int n)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    for (int i = 0; i < n; i++) {
        char l[120];
        snprintf(l, sizeof l, "nav: %s", path[i]);
        int ok;
        if (strcmp(path[i], "Songs") == 0) {   /* also the tail of "Shuffle Songs" */
            ok = select_row("Albums");
            hl_scroll(1);
            hl_run_ms(400);
        } else {
            ok = select_row(path[i]);
        }
        CHECK(l, ok);
        if (!ok) shot("nav");
        tap(HL_BTN_SELECT);
        hl_run_ms(1000);
    }
}
#define GO(...) do { const char *const p_[] = { __VA_ARGS__ }; \
                     go(p_, (int)(sizeof p_ / sizeof p_[0])); } while (0)

static void hold_select(void)
{
    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(800);
}

/* Long Player played from the album list (Long One first, 90 s). */
static void play_long_player(void)
{
    GO("Music", "Albums");
    CHECK("play: Long Player", select_row("Long Player"));
    tap(HL_BTN_PLAY);
    hl_run_ms(1500);
    CHECK("play: playing", hl_stats().audio_running && !player_paused());
}

/* The selected row's title is the marquee target: down the list from the top
 * until g_mq holds a title containing `s`. (The text search cannot read a
 * row drawn through the marquee.) */
static int select_marquee(const char *s)
{
    hl_scroll(-16);
    hl_run_ms(16 * 150 + 400);
    for (int i = 0; i < 24; i++) {
        if (g_mq.active && strstr(g_mq.text, s)) return 1;
        hl_scroll(1);
        hl_run_ms(450);
    }
    return g_mq.active && strstr(g_mq.text, s);
}

/* ---- cases ------------------------------------------------------------------ */

/* A marquee cycle is 1.5 s + (tw - w) x 26 ms + 0.9 s: Now Playing's title is
 * ~9 s of it, the Songs row's ~3 s; each run covers two cycles or more.
 * (Between runs a playing screen is NOT the full render of that instant: its
 * animators drew it up to a step ago. The frames are judged as they go out.) */

static void case_np(void)
{
    GO("Music", "Songs");
    CHECK("np: the long song", select_marquee("Overlong"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    CHECK("np: Now Playing", scr_cur() == SCR_NOWPLAYING);
    CHECK("np: playing", hl_stats().audio_running && !player_paused());
    marquee_run("np", 22000);
}

static void case_list(void)
{
    play_long_player();
    GO("Music", "Songs");
    CHECK("list: the long row", select_marquee("Overlong"));
    CHECK("list: still playing", hl_stats().audio_running && !player_paused());
    CHECK("list: Songs", scr_cur() == SCR_SONGS);
    marquee_run("list", 8000);
}

/* PLAY on the queue is play/pause. */
static void pause_here(void) { tap(HL_BTN_PLAY); }

static void case_queue(void)
{
    play_long_player();
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    CHECK("queue: Now Playing row", select_row("Now Playing"));
    tap(HL_BTN_SELECT);
    hl_run_ms(800);
    hold_select();
    hl_run_ms(500);
    CHECK("queue: the queue", scr_cur() == SCR_QUEUE && header("Now Playing")
          && in_list("Long One"));
    /* The playing row's bars, where queue_row_draw puts them (two rows: the
     * window does not scroll). */
    bars_run("queue", HL_W - 22, LIST_Y0 + player_queue_current() * ROW_H + 7,
             pause_here);
}

static void open_long_player(void)
{
    GO("Music", "Albums");
    CHECK("detail: Long Player", select_row("Long Player"));
    tap(HL_BTN_SELECT);                 /* into the album: its tracklist */
    hl_run_ms(1500);
    CHECK("detail: the tracklist", scr_cur() == SCR_BROWSER && g_dir_depth > 0
          && in_list("Long One"));
}

/* PLAY on the tracklist plays the selected row (and opens Now Playing): pause
 * from the main menu, where PLAY is play/pause, and come back. */
static void pause_from_menu(void)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    tap(HL_BTN_PLAY);
    open_long_player();
}

static void case_detail(void)
{
    play_long_player();
    open_long_player();
    /* Long One is the first row: the bars in its gutter (detail_row_draw). */
    bars_run("detail", 15, DET_LIST_Y0 + 6, pause_from_menu);
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
    printf("%s: fake time %.1f s, %u presents (%u full)\n", g_case,
           (double)s.now_us / 1e6, s.presents_full + s.presents_rect,
           s.presents_full);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);                     /* the firmware thread is frozen mid-wait */
}

typedef struct {
    const char *name;
    void (*run)(void);
} case_t;

static const case_t CASES[] = {
    { "np",     case_np },
    { "list",   case_list },
    { "queue",  case_queue },
    { "detail", case_detail },
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges [SHOT_DIR]\n", argv[0]);
        return 2;
    }
    g_shots = argc > 3 ? argv[3] : NULL;
    const char *only = getenv("ANIM_CASE");
    int ok = 1;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (only && strcmp(only, CASES[i].name) != 0) continue;
        char disk[] = "/tmp/animations_XXXXXX";
        int fd = mkstemp(disk);
        if (fd < 0 || copy_disk(argv[1], disk) != 0) {
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
                  load_ranges(argv[2]) >= 3);
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
            fprintf(stderr, "animations: %s FAILED\n", CASES[i].name);
            ok = 0;
        }
    }
    return ok ? 0 : 1;
}
