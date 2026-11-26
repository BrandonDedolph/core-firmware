/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/marquee.c — the one scrolling-text slot; see marquee.h.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/lcd.h"
#include "hal.h"
#include "../kernel/console.h"
#include "../ui/chrome.h"
#include "marquee.h"
#include "animpace.h"

marquee_t g_mq;

/* Marquee cadence: dwell showing the truncated start, scroll once to reveal the
 * tail, dwell on the tail, then reset to the start and repeat. `t_us` is time
 * SINCE the target was registered (g_mq.t0), so it always begins from the
 * truncated look rather than mid-scroll, and — because callers draw the title
 * through this at the SAME live offset — a full repaint never shows a truncated
 * frame that the scroll loop then has to correct (no wheel/lock stutter). */
#define MQ_US_PER_PX  26000u              /* ~38 px/s scroll                     */
#define MQ_HOLD_START 1500000u            /* dwell on the truncated start (1.5s) */
#define MQ_HOLD_END    900000u            /* dwell on the revealed tail   (0.9s) */

/* Current scroll offset (px) for overflowing text `tw` wide in a `w` window at
 * elapsed phase time `t_us`. 0 while it fits. */
static int mq_offset(int tw, int w, uint32_t t_us)
{
    if (tw <= w) return 0;
    int max_off = tw - w;
    uint32_t scroll = (uint32_t)max_off * MQ_US_PER_PX;
    uint32_t cycle  = MQ_HOLD_START + scroll + MQ_HOLD_END;
    uint32_t tp = t_us % cycle;
    int off;
    if (tp < MQ_HOLD_START) {
        off = 0;                                      /* truncated (start)      */
    } else if (tp < MQ_HOLD_START + scroll) {
        off = (int)((tp - MQ_HOLD_START) / MQ_US_PER_PX);
    } else {
        off = max_off;                                /* hold on the tail       */
    }
    if (off > max_off) off = max_off;
    return off;
}

/* Draw `s` in the window [x, x+w) at baseline y. Clears the text band to bg
 * first, then draws it at its current scroll offset (`t_us` = phase time). */
/* `tw` is the caller's already-measured text_width(s, font) — measuring it here
 * too made the marquee path walk the same string three times per frame. */
void draw_marquee(int x, int y, int w, const char *s, int tw,
                         const text_font_t *font, uint16_t ink, uint16_t bg,
                         uint32_t t_us, int cy0, int cy1)
{
    /* Clear only the glyph ink box (ascent+descent), NOT the full line_height —
     * line_height includes leading that reaches into a row's sub-line below, so
     * a scrolling title would otherwise erase the artist text under it. The
     * [cy0, cy1) window keeps the clear AND the glyphs inside the row, so tall
     * glyph tops (baseline sits close to the bar top) can't paint above the
     * selection bar into the row above. */
    int asc = text_ascent(font);
    int top = y - asc, bot = y + text_descent(font);
    if (top < cy0) top = cy0;
    if (bot > cy1) bot = cy1;
    if (bot > top) console_fill_rect(x, top, w, bot - top, bg);
    int off = mq_offset(tw, w, t_us);
    text_draw_clip_v(console_fb(), LCD_WIDTH, LCD_HEIGHT, x - off, y, s, font, ink,
                     x, x + w, cy0, cy1);
}

/* Register the selected/overflowing text as the marquee target (only if it
 * actually overflows its window). Resets the phase clock (g_mq.t0) when the
 * target changes — compared by CONTENT + position, since the now-playing title
 * reuses one buffer across tracks so a pointer check would miss the change — so
 * the scroll always restarts from the truncated look on a new row/track. */
static int mq_set(int x, int y, int w, const char *text, int tw,
                  const text_font_t *font, uint16_t ink, uint16_t bg,
                  int cy0, int cy1)
{
    if (tw <= w) return 0;

    /* Length-explicit: a target that doesn't FIT the fingerprint can't be
     * compared (its tail is the part that differs), so it would read "changed"
     * every frame and never scroll. Leave it static instead — it still renders
     * clipped, it just doesn't animate. */
    int len = 0;
    while (text[len]) {
        if (++len > MQ_TEXT_MAX) return 0;
    }

    int changed = (x != g_mq.last_x || y != g_mq.last_y);
    if (!changed) {
        int i = 0;
        while (i < len && text[i] == g_mq.last[i]) i++;
        changed = (i != len || g_mq.last[len] != '\0');
    }
    if (changed) {
        int i = 0;
        for (; i < len; i++) g_mq.last[i] = text[i];
        g_mq.last[i] = '\0';
        g_mq.last_x = x; g_mq.last_y = y;
        g_mq.t0 = mmio_read32(USEC_TIMER_ADDR);
    }
    g_mq.active = 1; g_mq.x = x; g_mq.y = y; g_mq.w = w; g_mq.tw = tw;
    /* Point at our OWN copy (g_mq.last), not the caller's buffer: some callers
     * (e.g. the album list) build the row text in a per-iteration stack local,
     * which is dead by the time the scroll tick re-reads it — a dangling pointer
     * that painted a different album's name. last[] is static and always holds
     * the current target's content, so the periodic redraw stays valid. */
    g_mq.text = g_mq.last; g_mq.font = font; g_mq.ink = ink; g_mq.bg = bg;
    g_mq.cy0 = cy0; g_mq.cy1 = cy1;
    return 1;
}

/* Draw `text` in [x, x+w) at baseline y AND register it as the marquee target.
 * Overflowing text is rendered at its current scroll offset (not a static
 * offset-0), so a full repaint mid-scroll shows the live position rather than a
 * truncated frame the scroll loop then jumps to correct. Non-overflowing text
 * just draws normally. Clears the text band to bg first. */
void mq_text(int x, int y, int w, const char *text,
                    const text_font_t *font, uint16_t ink, uint16_t bg,
                    int cy0, int cy1)
{
    /* Measure ONCE and pass it down (mq_set + draw_marquee both need it). */
    int tw = text_width(text, font);
    if (tw <= w || !mq_set(x, y, w, text, tw, font, ink, bg, cy0, cy1)) {
        /* Fits (or is too long to marquee safely): just draw it. NO background
         * band clear — the caller already painted the row/selection background,
         * and a clear at y-ascent would bleed ABOVE the selection bar (ascent 14 >
         * the sub-row title lift of 11) and paint a black bar over the row above. */
        ui_text_clip(x, y, text, font, ink, x, x + w);
        return;
    }
    draw_marquee(x, y, w, text, tw, font, ink, bg,
                 mmio_read32(USEC_TIMER_ADDR) - g_mq.t0, cy0, cy1);
}

/* run_ui's marquee step: redraw g_mq's band at the live phase offset and
 * partial-present it, once MQ_STEP_US has passed since L->last_mq
 * (app/animpace.h). run_ui calls it while g_mq.active and the backlight is
 * on. */
void marquee_tick(ui_loop_t *L)
{
    uint32_t nowm = mmio_read32(USEC_TIMER_ADDR);
    /* Phase clock (g_mq.t0) + target-change detection live in mq_set, so
     * this just paces the redraw at the shared live offset. */
    if (marquee_step_due(nowm, L->last_mq)) {   /* ~30fps               */
        draw_marquee(g_mq.x, g_mq.y, g_mq.w, g_mq.text, g_mq.tw, g_mq.font,
                     g_mq.ink, g_mq.bg, nowm - g_mq.t0, g_mq.cy0, g_mq.cy1);
        int ptop = g_mq.y - text_ascent(g_mq.font);
        int pbot = g_mq.y + text_descent(g_mq.font);
        if (ptop < 0) ptop = 0;
        lcd_present_rect(console_framebuffer(),
                         g_mq.x, ptop, g_mq.w, pbot - ptop);
        L->last_mq = nowm;
    }
}
