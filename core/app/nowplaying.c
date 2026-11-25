/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/nowplaying.c — see nowplaying.h.
 */

#include <stdint.h>

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hal.h"
#include "../kernel/console.h"
#include "../player/player.h"
#include "../library/names.h"
#include "../ui/text.h"
#include "../ui/thumb.h"
#include "../ui/palette.h"
#include "../ui/chrome.h"
#include "../ui/gesture.h"
#include "../ui/sleeptimer.h"
#include "state.h"
#include "util.h"
#include "marquee.h"
#include "battery_ui.h"
#include "statusbar.h"
#include "nowplaying.h"

static int      g_np_scrub;          /* 1 = wheel seeks instead of volume      */
uint32_t g_scrub_target_s;    /* where the wheel is currently aiming    */
uint32_t g_scrub_last_us;     /* last detent, for commit/exit timing    */
int      g_scrub_dirty;       /* target moved since the last commit     */

uint32_t g_sel_down_us;
int      g_sel_pending;

int np_scrubbing(void)
{
    return g_np_scrub;
}

void scrub_enter(void)
{
    g_np_scrub       = 1;
    g_scrub_target_s = player_elapsed_s();
    g_scrub_last_us  = mmio_read32(USEC_TIMER_ADDR);
    g_scrub_dirty    = 0;
}

void scrub_exit(void)
{
    g_np_scrub    = 0;
    g_scrub_dirty = 0;
}

/*
 * Hold RIGHT / LEFT on the player screens: fast forward and rewind. The same
 * aim-then-commit trade the wheel scrubber makes above, for the same reason —
 * a seek per tick would stop the DAC, re-prime the ring and rewind the file
 * four times a second. ui/gesture.c owns the press-length decision, the ramp
 * and the clamps; run_ui() only feeds them and acts on what comes back.
 *
 * File-scope rather than run_ui() locals because the transport band renderer
 * (well above run_ui) has to know where the hold is aiming.
 */
seekhold_t g_ff = { .dir = +1 };
seekhold_t g_rw = { .dir = -1 };

/*
 * Where the transport band should point: the wheel scrubber's target, a
 * RIGHT/LEFT hold's, or nothing at all — in which case the band shows the
 * live position. One question, asked once, so the renderer stays ignorant of
 * which gesture is aiming.
 */
static int np_aim_target(uint32_t *target)
{
    if (np_scrubbing())         { *target = g_scrub_target_s;      return 1; }
    if (seekhold_active(&g_ff)) { *target = seekhold_target(&g_ff); return 1; }
    if (seekhold_active(&g_rw)) { *target = seekhold_target(&g_rw); return 1; }
    return 0;
}

/* Right-side circle arc (a ")" shape): radius R, +/- span rows tall, centred at
 * (cx, cy); ~2px thick. The speaker's sound waves + the lock shackle use it. */
/* A 1px-thin right-opening crescent (radius R, +/-span rows), anchored so its
 * near point sits by the speaker cone — the skinny sound wave. */
static void draw_arc_thin(int cx, int cy, int R, int span, uint16_t c)
{
    for (int dy = -span; dy <= span; dy++) {
        int dx = ui_isqrt(R * R - dy * dy);
        console_fill_rect(cx + dx, cy + dy, 1, 1, c);
    }
}

/* A small right-pointing speaker: driver cabinet + cone, then sound waves that
 * scale with `vol` — two waves loud, one wave quiet, and a mute "X" at 0. */
static void draw_speaker(int sx, int sy, uint16_t c, int vol)
{
    ui_round_rect(sx - 8, sy - 3, 4, 6, 1, c);          /* cabinet             */
    for (int dx = 0; dx <= 4; dx++) {                     /* cone, opening right */
        int half = 2 + dx;
        console_fill_rect(sx - 4 + dx, sy - half, 1, 2 * half, c);
    }
    if (vol <= 0) {                                       /* muted: an X         */
        for (int i = 0; i < 6; i++) {
            console_fill_rect(sx + 3 + i, sy - 3 + i, 2, 1, c);   /* '\'         */
            console_fill_rect(sx + 3 + i, sy + 2 - i, 2, 1, c);   /* '/'         */
        }
        return;
    }
    /* Three skinny (1px) crescent waves anchored at the cone front, growing with
     * volume — closer + thinner than the old 2px arcs (chosen design "B"). */
    if (vol > 5)  draw_arc_thin(sx, sy, 3, 2, c);         /* wave 1: hairline    */
    if (vol > 40) draw_arc_thin(sx, sy, 6, 4, c);         /* wave 2              */
    if (vol > 72) draw_arc_thin(sx, sy, 9, 5, c);         /* wave 3              */
}

/* Centered volume overlay plate (volume-demo.jsx VolumeOverlay): speaker glyph +
 * ink fill bar + big percent, on a light near-surface plate. Everything it
 * draws stays inside the VOL_PLATE_* rect (chrome.h) — the main loop presents
 * exactly that rect for a volume tick, so ink outside it would never reach
 * the panel.
 *
 * The bar keeps its 0..100 scale whatever the Volume Limit is, and a limit
 * below 100 puts a small triangle over the track at the ceiling — the marker
 * the 5G's own Features Guide describes, and the only thing on screen that
 * explains why the wheel stopped. */
static void volume_overlay_render(int vol, int limit)
{
    const int PX = VOL_PLATE_X, PY = VOL_PLATE_Y, PW = VOL_PLATE_W, PH = VOL_PLATE_H;
    ui_round_rect(PX, PY, PW, PH, 8, LINEN_PLATE);    /* raised plate, AA r8  */

    draw_speaker(PX + 16, PY + PH / 2, LINEN_INK, vol);

    /* Fill bar. */
    int bx = PX + 34, by = PY + PH / 2 - 3, bw = PW - 34 - 42, bh = 6;
    console_fill_rect(bx, by, bw, bh, LINEN_TRK);          /* track rgba(ink,0.12) */
    int fw = bw * vol / 100;
    if (fw < 0) fw = 0;
    if (fw > bw) fw = bw;
    console_fill_rect(bx, by, fw, bh, LINEN_INK);

    /* Ceiling marker: a 5x3 triangle, apex down, sitting ON the track's top
     * edge. Rows PY+9..PY+11 and x in [bx-2, bx+bw+2] are both well inside
     * the plate, so the present rect the caller pushes still covers every
     * pixel drawn here. The fill can never reach past it (vol <= limit by
     * construction), so the two never collide. */
    if (limit < 100) {
        int mx = bx + bw * limit / 100;
        console_fill_rect(mx - 2, by - 4, 5, 1, LINEN_INK);
        console_fill_rect(mx - 1, by - 3, 3, 1, LINEN_INK);
        console_fill_rect(mx,     by - 2, 1, 1, LINEN_INK);
    }

    /* Percent, right-aligned. */
    char p[5];
    u32_to_dec(p, (unsigned)vol);
    int w = text_width(p, text_font_bold_12());       /* percent is 11/700       */
    ui_text(PX + PW - 14 - w, PY + PH / 2 + 4, p, text_font_bold_12(), LINEN_INK);
}

/* Now-playing album art at 120x120 — the stored folder.art's native size, so it
 * blits straight through at full quality (no downscale) and fills the art-left
 * layout. Only art larger than this is box-downscaled (cached per track). */
#define NP_ART_DIM 120
static uint16_t g_np_art[NP_ART_DIM * NP_ART_DIM];
static int      g_np_art_key = -1;

static const uint16_t *np_art_120(void)
{
    if (!player_art_ok()) { g_np_art_key = -1; return 0; }
    int w = player_art_w(), h = player_art_h();
    if (w == NP_ART_DIM && h == NP_ART_DIM) {
        return player_art_pixels();        /* native size: blit as-is */
    }
    /* Keyed on the ART generation, not the queue index: a freshly built queue
     * that happens to start on the same index would otherwise re-use the
     * PREVIOUS album's downscaled cover. player_art_seq() bumps whenever the
     * art changes, including when it is cleared. */
    int key = (int)player_art_seq();
    if (key != g_np_art_key) {
        thumb_box_rgb565(player_art_pixels(), w, h,
                         g_np_art, NP_ART_DIM, NP_ART_DIM);
        g_np_art_key = key;
    }
    return g_np_art;
}

/* Now-playing TRANSPORT strip: the elapsed / −remaining times and the progress
 * bar, i.e. everything that changes once a second. Split out of the full
 * renderer because the rest of the screen (art + metadata) only changes on a
 * track change: the clock tick redraws THIS band and presents only it, instead
 * of clearing the framebuffer, re-blitting the 120x120 cover and rebuilding
 * every metadata string every second and discarding all of it above y=128. */
/* NP_TR_Y / NP_TR_H (the band's rect) live in ui/chrome.h so the host present
 * test can pin the word count this band costs on the BCM. */

void nowplaying_transport_render(uint32_t elapsed_s, uint32_t total_s)
{
    console_fill_rect(0, NP_TR_Y, LCD_WIDTH, NP_TR_H, LINEN_SURFACE);

    /* While an aim is in flight — the wheel scrubber, or a RIGHT/LEFT hold —
     * the bar and the left-hand time show the TARGET, not the live position:
     * the gesture is aiming at a destination and the readout has to be what
     * you are aiming at. The right-hand side becomes the delta from where
     * playback actually is, so a long seek is legible ("+3:41") instead of a
     * remaining figure you have to subtract in your head. */
    uint32_t aim    = 0;
    int      aiming = np_aim_target(&aim);
    uint32_t shown  = aiming ? aim : elapsed_s;

    char te[FMT_TIME_MAX], tr[FMT_TIME_MAX + 2];   /* tr carries a sign prefix */
    fmt_time(te, shown);
    if (aiming) {
        uint32_t d = (shown > elapsed_s) ? shown - elapsed_s : elapsed_s - shown;
        tr[0] = (shown >= elapsed_s) ? '+' : '-';
        fmt_time(tr + 1, d);
    } else {
        uint32_t rem = (total_s > elapsed_s) ? total_s - elapsed_s : 0;
        tr[0] = '-';
        fmt_time(tr + 1, rem);                         /* "−M:SS" remaining     */
    }
    ui_text(18, 198, te, FONT_SUB, aiming ? LINEN_INK : LINEN_MUTED_D);
    int wtr = text_width(tr, FONT_SUB);
    ui_text(LCD_WIDTH - 18 - wtr, 198, tr, FONT_SUB,
            aiming ? LINEN_INK : LINEN_MUTED_D);

    /* Taller rounded-cap bar (INK fill on a faint ink track, Theme1Live fg).
     * bh 8 with AA pill caps reads smoother than the old 6px integer-stepped
     * caps. */
    int pbx = 18, by = 209, bw = LCD_WIDTH - 36, bh = 8;
    ui_round_rect(pbx, by, bw, bh, bh / 2, LINEN_TRK);
    int fw = (total_s > 0) ? (int)((shown * (uint32_t)bw) / total_s) : 0;
    if (fw > bw) fw = bw;
    if (fw >= bh) {
        ui_round_rect(pbx, by, fw, bh, bh / 2, LINEN_INK);
    } else if (fw > 0) {
        console_fill_rect(pbx, by, fw, bh, LINEN_INK);
    }
    /* A playhead at the target so the aim point is readable even where the
     * filled length is ambiguous (very short or very long tracks). */
    if (aiming) {
        int hx = pbx + fw - 1;
        if (hx < pbx)          hx = pbx;
        if (hx > pbx + bw - 3) hx = pbx + bw - 3;
        console_fill_rect(hx, by - 3, 3, bh + 6, LINEN_INK);
    }
}

/* Now-playing (Linen, interactive-ipod.jsx Theme1Live): a top status row
 * ("Now Playing"/"Paused" + shuffle/repeat + battery), then 88x88 art on the
 * LEFT with a metadata column to its right (SONG N OF M / title / artist /
 * album), and the elapsed / −remaining times + progress bar pinned to the
 * bottom. Art + metadata come from the PLAYING folder, not the browsed one. */
void nowplaying_render(const char *name, uint32_t elapsed_s,
                              uint32_t total_s, uint32_t buf_pct)
{
    (void)buf_pct;
    console_clear(LINEN_SURFACE);

    /* --- top status row: context label left, state + battery right --------- */
    ui_text(12, 15, player_paused() ? "Paused" : "Now Playing",
            text_font_bold_12(), LINEN_INK);

    int bx = LCD_WIDTH - 12 - 19;                     /* battery block         */
    draw_battery(bx, 3, g_bat_pct);
    int rc = bx - (g_locked ? 18 : 6);                /* right edge for tokens */
    if (g_locked) {
        draw_lock_glyph(bx - 14, 3, LINEN_INK);
    }
    /* Compact shuffle / repeat / sleep tokens, right-aligned before the
     * battery. Built as ONE string and measured once, so the spacing between
     * them is the font's own kerning rather than three guessed offsets. Worst
     * case "SHUF·ALB RPT1 SLEEP 120" is 128 px against the ~194 px of air this
     * band has beside "Now Playing" (23 bytes into st[32], too). */
    {
        char st[32]; int p = 0;
        if (g_settings.shuffle) {
            /* SHUF·ALB only when albums is what is actually being walked: a
             * Shuffle Songs queue IS its own song order (PLAYER_KEEP_QUEUE),
             * and the setting does not regroup it, so the token must not
             * claim otherwise. */
            const char *s = (g_settings.shuffle == SHUFFLE_ALBUMS &&
                             player_order_keep() != PLAYER_KEEP_QUEUE)
                              ? "SHUF" UI_GLYPH_MIDDOT "ALB" : "SHUF";
            while (*s) st[p++] = *s++;
        }
        if (g_settings.repeat != REPEAT_OFF) {
            if (p) st[p++] = ' ';
            const char *s = (g_settings.repeat == REPEAT_ONE) ? "RPT1" : "RPT";
            while (*s) st[p++] = *s++;
        }
        {
            char tok[12];                    /* "" while the timer is off */
            if (sleeptimer_token(&g_sleep, tok, (int)sizeof tok) > 0) {
                if (p) st[p++] = ' ';
                const char *s = tok; while (*s) st[p++] = *s++;
            }
        }
        st[p] = '\0';
        if (p) {
            int w = text_width(st, FONT_SMALL);
            ui_text(rc - w, 13, st, FONT_SMALL, LINEN_MUTED2);
        }
    }

    /* --- art on the left (native 120x120) ---------------------------------- */
    const int ax = 16, ay = 44;
    const uint16_t *art = np_art_120();
    if (art) {
        console_blit565(ax, ay, NP_ART_DIM, NP_ART_DIM, art);
    } else {
        /* No cover: a neutral tile, same as the list rows' chip placeholder —
         * bare surface here read as a rendering bug, not as "no artwork". */
        console_fill_rect(ax, ay, NP_ART_DIM, NP_ART_DIM, LINEN_BORDER);
    }

    /* --- metadata column, vertically centred beside the taller art --------- */
    const flac_meta_t *m = player_meta();
    const char *title = (m->have && m->title[0]) ? m->title : track_display(name);
    int mx = ax + NP_ART_DIM + 14;                    /* metadata left edge    */
    int mr = LCD_WIDTH - 14;                           /* metadata right edge   */

    /* "TRACK N OF M" eyebrow above the title (design's "Track 04 of 11"). */
    int tot = player_queue_len();
    if (tot > 0) {
        /* 8 + 12 literal chars + two decimals (<=10 digits each) + NUL. The old
         * 24-byte buffer only covered ONE-digit numbers on both sides: any album
         * with >=10 tracks overran it, and a 1200-entry shuffle queue by 5. */
        char num[8 + 12 + 10 + 10 + 1];
        int p = 0;
        for (const char *q = "TRACK   "; *q; q++) num[p++] = *q;   /* air after TRACK */
        p += u32_to_dec(num + p, (unsigned)(player_queue_current() + 1));
        for (const char *q = "     OF     "; *q; q++) num[p++] = *q;  /* extra air both sides */
        p += u32_to_dec(num + p, (unsigned)tot);
        num[p] = '\0';
        ui_text(mx, 72, num, FONT_SMALL, LINEN_MUTED2);
    }

    /* Title (bold 17): drawn at its live scroll offset + registered as the
     * marquee, so volume/lock full repaints never flash the truncated title. */
    mq_text(mx, 94, mr - mx, title, FONT_TITLE, LINEN_INK, LINEN_SURFACE,
            0, LCD_HEIGHT);   /* now-playing title has room — no row clip */

    if (m->have && m->artist[0]) {
        ui_text_clip(mx, 114, m->artist, FONT_SUB, LINEN_MUTED_D, mx, mr);
    }
    if (m->have && m->album[0]) {
        ui_text_clip(mx, 130, m->album, FONT_SUB, LINEN_MUTED2, mx, mr);
    }

    nowplaying_transport_render(elapsed_s, total_s);

    /* Volume overlay rides on top for ~1.5 s after a wheel adjustment. */
    if (ui_window_up(&g_vol_show, VOL_SHOW_US, mmio_read32(USEC_TIMER_ADDR))) {
        volume_overlay_render(g_volume, g_settings.volume_limit);
    }
}
