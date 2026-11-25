/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/statusbar.c — see statusbar.h.
 */

#include <stdint.h>

#include "hal.h"
#include "../kernel/console.h"
#include "../kernel/datetime.h"
#include "../player/player.h"
#include "../library/names.h"
#include "../ui/text.h"
#include "../ui/palette.h"
#include "../ui/chrome.h"
#include "../ui/sleeptimer.h"
#include "state.h"
#include "layout.h"
#include "clockui.h"
#include "nav.h"
#include "battery_ui.h"
#include "otg.h"
#include "statusbar.h"

/* Small padlock glyph (system-screens.jsx corner lock): body + shackle. */
void draw_lock_glyph(int x, int y, uint16_t c)
{
    console_fill_rect(x,     y + 4, 8, 6, c);   /* body      */
    console_fill_rect(x + 1, y,     2, 5, c);   /* left post */
    console_fill_rect(x + 5, y,     2, 5, c);   /* right post*/
    console_fill_rect(x + 1, y,     6, 2, c);   /* top arch  */
}

/*
 * Where the strip's right-hand cluster ends for anything drawn LEFT of the
 * battery: past the padlock while Hold is on, so the token never sits on it.
 * One function because BOTH strip painters — the real one and the Hold
 * banner's recoloured copy — have to agree: the banner draws no padlock, but
 * it is up precisely while Hold is flipping, and a token that moved 14 px
 * when the banner faded would hop once per flip. The gap the missing padlock
 * leaves for that second is the cheaper of the two.
 */
static int strip_cluster_left(void)
{
    int bx = LCD_WIDTH - 12 - 24;             /* battery block (22 + 2 nub)  */
    return bx - (g_locked ? 14 + 6 : 6);      /* past the padlock at bx-14   */
}

/*
 * The sleep timer's token ("SLEEP 45") right-aligned at `right_x`, in the
 * strip's small font. Returns the LEFT edge of what it drew, so the caller
 * can clip whatever sits to its left — or `right_x` untouched when the timer
 * is off, which is the usual case and costs nothing.
 */
static int strip_sleep_token(int right_x, int y, uint16_t ink)
{
    char tok[12];
    if (sleeptimer_token(&g_sleep, tok, (int)sizeof tok) <= 0) {
        return right_x;
    }
    int w = text_width(tok, FONT_SMALL);
    ui_text(right_x - w, y, tok, FONT_SMALL, ink);
    return right_x - w;
}

/*
 * The STRIP's left slot, shared by the real strip and the Hold banner's
 * recoloured copy of it.
 *
 * The track name wins whenever something is playing. That is the 2026-09-14
 * decision — the strip is a now-playing readout — and it is also arithmetic:
 * the left slot clips at 238 px and a 12-hour clock costs ~38 of them, so
 * showing both would take a third of every title away permanently in exchange
 * for a readout the main menu's header already carries.
 */
static const char *strip_left_text(char *buf, int buf_sz)
{
    if (player_active()) {
        return track_display(player_track_name());
    }
    clock_title_text(buf, buf_sz);
    return buf;
}

/* The top status strip: the now-playing track name on the left (so you always
 * see what's playing while browsing), battery on the right. During bring-up the
 * right side also shows raw millivolts (to calibrate the %-curve; see
 * battery.h "DEVICE-GATED CALIBRATION"). */
void status_strip_render(void)
{
    /* Left: the playing track, else the clock when Time in Title is on, else
     * NOTHING — the strip is a now-playing readout, not a wordmark (the main
     * menu's own header already says "Core"). Clipped by the right cluster,
     * which is painted over it. */
    char clock[DATETIME_TIME_MAX];
    const char *left = strip_left_text(clock, (int)sizeof clock);

    int bx = LCD_WIDTH - 12 - 24;             /* battery block (22 + 2 nub)        */

    /* The sleep timer's token goes LEFT of the padlock, which sits left of the
     * battery — so neither of those two ever moves when the timer is armed;
     * only the name gives ground. Drawn first, because the name's clip has to
     * know where it starts. */
    int tok_x = strip_sleep_token(strip_cluster_left(), STATUS_Y0 + 11,
                                  LINEN_MUTED2);

    /* CLIP the name before the right-hand cluster rather than drawing it full
     * width and then painting a 70x15 rectangle back over its tail — same look,
     * without rasterising glyphs that are immediately overwritten (and without
     * dirtying that band for a partial present). */
    if (left[0]) {
        int clip_r = LCD_WIDTH - 70;
        if (tok_x - 8 < clip_r) {
            clip_r = tok_x - 8;               /* a long name loses the room     */
        }
        ui_text_clip(12, STATUS_Y0 + 11, left, FONT_SMALL, LINEN_MUTED2,
                     12, clip_r);
    }

    draw_battery(bx, STATUS_Y0 + 1, g_bat_pct);

    /* Persistent padlock while Hold is engaged (design keeps it in the strip). */
    if (g_locked) {
        draw_lock_glyph(bx - 14, STATUS_Y0 + 3, LINEN_INK);
    }

    /* Raw mV to the left of the glyph — a device-gated CALIBRATION aid, not part
     * of the design. Off for the shipping look; flip SHOW_BATTERY_MV to 1 to
     * read the millivolts and calibrate battery.c's %-curve (see battery.h). */
#define SHOW_BATTERY_MV 0
    if (SHOW_BATTERY_MV && g_bat_mv > 0 && !g_locked) {
        char mv[8];
        int v = g_bat_mv, i = 0;
        char tmp[8]; int t = 0;
        if (v > 9999) v = 9999;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 7);
        while (t > 0) mv[i++] = tmp[--t];
        mv[i] = '\0';
        int w = text_width(mv, FONT_SMALL);
        ui_text(bx - 5 - w, STATUS_Y0 + 11, mv, FONT_SMALL, LINEN_MUTED2);
    }
}

/* Titled header with an optional back chevron and a right-aligned count/value,
 * plus the divider under it (menus.jsx ScreenHeader). */
/* Convenience: a single-line (24px) row. */
void list_row(int r, const char *text, const char *sub, const char *right,
                     int chevron, int selected, int greyed, const uint16_t *chip)
{
    ui_list_row(LIST_Y0, r, text, sub, right, chevron, selected, greyed, chip, 0, ROW_H);
}

/* A taller (28px) two-line row for the album list: title + artist sub with a
 * cover chip, so the two lines don't overlap and the marquee stays title-only. */
void list_row_tall(int r, const char *text, const char *sub, const char *right,
                          int chevron, int selected, int greyed, const uint16_t *chip)
{
    ui_list_row(LIST_Y0, r, text, sub, right, chevron, selected, greyed, chip, 0, ROW_H2);
}

/* Like list_row_tall, but the title takes priority over the right-hand value: a
 * long title spans the full width (over the value) and marquees on select. */
void list_row_titled(int r, const char *text, const char *sub,
                            const char *right, int selected, const uint16_t *chip)
{
    ui_list_row(LIST_Y0, r, text, sub, right, 0, selected, 0, chip, 1, ROW_H2);
}

/* ---------------------------------------------------------------------------
 * Top banner — the Hold "plate"; the primitive is reusable if another
 * announcement ever needs the top chrome
 *
 * A Hold edge does not raise a card over the screen (the reference jsx's
 * centred 180x110 plate, which covered the title and half the art for one
 * bit of information). The TOP CHROME INVERTS for LOCK_FLASH_US and then
 * settles back into the persistent strip padlock. Both states draw on the
 * surface with the ordinary border rule under the band; the closed or open
 * padlock and the words carry the difference. (The primitive can still
 * invert — `inverted` picks the selected-row pair — but the Hold banner no
 * longer does: the locked/unlocked flip read as jarring on the device.)
 *
 * The band is whatever the top chrome is on the current screen:
 *   - Now Playing: the 22 px status row, plus the border rule under it when
 *     the band is not inverted;
 *   - list and Settings screens: status strip + header, through the divider
 *     (39 px); the header line carries the announcement where the title was
 *     and the strip row above keeps its track name (blank when idle) and
 *     battery, recoloured.
 * The battery never leaves — the right cluster does not flicker.
 *
 * The dark modals (CHARGING, BATTERY) host no banner at all. They have no
 * chrome: the band there was a linen strip over the near-black field, with
 * the STRIP's little battery token blinking in above a screen that is
 * already one big battery — on the device it read as a nonsense animation.
 * Hold still locks the wheel there; what shows is the corner padlock the
 * modal paints itself (screen_charging_lock_render), the persistent glyph
 * without the flash. banner_hosted() is the one place that rule lives.
 *
 * Design: docs/screens lock.png / locked.png / locked_list.png, from the
 * 2026-09-14 "Hold Plate Redesign" review (pill, banner and card compared;
 * banner chosen, dot-keyhole padlock, pop-open).
 * ------------------------------------------------------------------------- */

/* Dot-keyhole padlock, 14 px wide, as row bitmasks (bit 13 = left column).
 * Closed = 16 rows; open = 18 rows, the shackle "popped" clear of the body
 * with both legs floating. Blitted as horizontal runs. The 8x10 strip glyph
 * (draw_lock_glyph) is its persistent little sibling. */
static const uint16_t LOCK_BM_CLOSED[16] = {
    0x03F0, 0x07F8, 0x0E1C, 0x0C0C, 0x0C0C, 0x0C0C,
    0x1FFE, 0x3FFF, 0x3FFF, 0x3F3F, 0x3F3F, 0x3FFF,
    0x3FFF, 0x3FFF, 0x3FFF, 0x1FFE,
};
static const uint16_t LOCK_BM_OPEN[18] = {
    0x03F0, 0x07F8, 0x0E1C, 0x0C0C, 0x0C0C, 0x0C0C,
    0x0000, 0x0000, 0x1FFE, 0x3FFF, 0x3FFF, 0x3F3F,
    0x3F3F, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x1FFE,
};

/* Blit a 14-wide row-mask bitmap at (x, y) as horizontal runs. */
static void draw_bitmap14(int x, int y, const uint16_t *rows, int n, uint16_t c)
{
    for (int r = 0; r < n; r++) {
        uint16_t m   = rows[r];
        int      run = -1;
        for (int col = 0; col <= 14; col++) {
            int on = (col < 14) && (m & (1u << (13 - col)));
            if (on && run < 0) {
                run = col;
            } else if (!on && run >= 0) {
                console_fill_rect(x + run, y + r, col - run, 1, c);
                run = -1;
            }
        }
    }
}

/* draw_battery in explicit colours, so it can sit on an inverted band. */
static void draw_battery_c(int x, int y, int pct, uint16_t outline, uint16_t fill)
{
    const int w = 22, h = 12;
    console_fill_rect(x, y, w, 1, outline);
    console_fill_rect(x, y + h - 1, w, 1, outline);
    console_fill_rect(x, y, 1, h, outline);
    console_fill_rect(x + w - 1, y, 1, h, outline);
    console_fill_rect(x + w, y + 4, 2, h - 8, outline);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int fw = ((w - 4) * pct) / 100;
    if (fw > 0) {
        console_fill_rect(x + 2, y + 2, fw, h - 4, pct <= 20 ? BATT_LOW_RED : fill);
    }
}

/* Whether the current screen has top chrome for a banner to take. The dark
 * modals do not (see above): a Hold edge or a refused press there must not
 * arm the flash, and a flash armed elsewhere is dropped the moment a modal
 * is up, so nothing stale is replayed over the screen the modal pops to. */
int banner_hosted(void)
{
    return !scr_is_modal(scr_cur());
}

/* Height of the band a banner inverts on the current screen (see above). The
 * present of a banner is exactly this band when nothing else is pending.
 * Only meaningful where banner_hosted(). */
int top_banner_h(void)
{
    switch (scr_cur()) {
    case SCR_NOWPLAYING:
        return 23;                        /* the 22 px row + the rule under it */
    default:
        return HDR_DIV_Y + 1;
    }
}

/* Paint a banner over the top chrome of whatever is in the framebuffer:
 * `inverted` picks the selected-row pair (else surface + border rule); `bm`
 * is a 14-wide bitmap of `bn` rows, drawn `bm_dy` px below where a 16-row
 * glyph sits (so a taller open padlock keeps its body in place); `label` is
 * the announcement; `token` (may be NULL) is the small-caps right-hand value
 * on the list variant, where the header count was. The strip row keeps the
 * chrome's own text (the track name, or nothing when idle). The label is
 * ellipsised to the room left of the token, as ui_header does for a title
 * next to its count. */
static void top_banner_render(int inverted, const uint16_t *bm, int bn, int bm_dy,
                              const char *label, const char *token)
{
    uint16_t band = inverted ? LINEN_INK     : LINEN_SURFACE;
    uint16_t fg   = inverted ? LINEN_SURFACE : LINEN_INK;
    uint16_t sub  = inverted ? LINEN_SEL_SUB : LINEN_MUTED2;
    int      h    = top_banner_h();

    if (h == 23) {
        /* Now Playing's own top row: glyph + label left, battery right. A
         * surface band on the surface needs an edge to read as a banner at
         * all, so the un-inverted one gets the border rule under it (the
         * inverted band is its own edge; its row 22 stays the surface). */
        console_fill_rect(0, 0, LCD_WIDTH, 22, band);
        if (!inverted) {
            console_fill_rect(0, 22, LCD_WIDTH, 1, LINEN_BORDER);
        }
        draw_bitmap14(12, 3 + bm_dy, bm, bn, fg);
        ui_text(12 + 14 + 6, 15, label, text_font_bold_12(), fg);
        draw_battery_c(LCD_WIDTH - 12 - 19, 3, g_bat_pct, sub, fg);
        return;
    }
    console_fill_rect(0, 0, LCD_WIDTH, h, band);
    /* Strip row, recoloured (every screen with a strip — Settings included,
     * where main.c paints one over the painter's clear band), then the header
     * line as the announcement, then the divider in the band's own secondary
     * colour so the inverted block ends where the header does. */
    {
        char clock[DATETIME_TIME_MAX];
        const char *left = strip_left_text(clock, (int)sizeof clock);
        /* The banner owns the whole strip row for its second, so it has to
         * redraw the SLEEP token too (in the band's own secondary colour) or
         * the Hold banner would blink the countdown off. No padlock is drawn
         * here — the banner IS the padlock — but the token still takes the
         * strip's own x (strip_cluster_left) so it does not hop when the
         * banner fades. */
        int tok_x = strip_sleep_token(strip_cluster_left(), STATUS_Y0 + 11, sub);
        if (left[0]) {
            int clip_r = LCD_WIDTH - 70;
            if (tok_x - 8 < clip_r) {
                clip_r = tok_x - 8;
            }
            ui_text_clip(12, STATUS_Y0 + 11, left, FONT_SMALL, sub, 12, clip_r);
        }
        draw_battery_c(LCD_WIDTH - 12 - 24, STATUS_Y0 + 1, g_bat_pct, sub, fg);
    }
    draw_bitmap14(12, HDR_BASE - 12 + bm_dy, bm, bn, fg);
    int lx        = 12 + 14 + 6;
    int show_tok  = (token && token[0]);
    int tok_w     = show_tok ? text_width(token, FONT_SMALL) : 0;
    int label_max = show_tok ? (LCD_WIDTH - 12 - tok_w - 8) - lx
                             : (LCD_WIDTH - 12) - lx;
    ui_text_ellipsis(lx, HDR_BASE, label, FONT_HEADER, fg, label_max);
    if (show_tok) {
        ui_text(LCD_WIDTH - 12 - tok_w, HDR_BASE - 1, token, FONT_SMALL, sub);
    }
    console_fill_rect(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, inverted ? sub : LINEN_BORDER);
}

/* The Hold banner: closed padlock + "Locked" / HOLD ON, or popped-open padlock
 * + "Unlocked" / HOLD OFF — both on the surface with the border rule under
 * them. The locked one was the inverted (selected-row) pair through v0.1.1;
 * on the device the flip between the two read as jarring, so the padlock and
 * the words carry the difference now (v0.1.2). */
void lock_banner_render(int locked)
{
    if (locked) {
        top_banner_render(0, LOCK_BM_CLOSED, 16,  0, "Locked",   "HOLD ON");
    } else {
        top_banner_render(0, LOCK_BM_OPEN,   18, -1, "Unlocked", "HOLD OFF");
    }
}

/* A "+" and a "-" in a ring, 14 px wide, drawn in the same rows-of-bits style
 * as the padlock above: what the On-The-Go banner puts where the padlock goes.
 * Added and removed are the only two things it ever says. */
static const uint16_t OTG_BM_PLUS[16] = {
    0x0000, 0x03F0, 0x07F8, 0x0E1C, 0x1806, 0x38C7, 0x30C3, 0x33F3,
    0x33F3, 0x30C3, 0x38C7, 0x1806, 0x0E1C, 0x07F8, 0x03F0, 0x0000,
};
static const uint16_t OTG_BM_MINUS[16] = {
    0x0000, 0x03F0, 0x07F8, 0x0E1C, 0x1806, 0x3807, 0x3003, 0x33F3,
    0x33F3, 0x3003, 0x3807, 0x1806, 0x0E1C, 0x07F8, 0x03F0, 0x0000,
};

/*
 * The On-The-Go banner. Same primitive as the Hold banner, same band, and
 * deliberately not modal: a refusal ("Not in the library", "On-The-Go is
 * full") has no glyph at all, because a + on a refusal would be a lie.
 */
void otg_banner_render(void)
{
    const uint16_t *bm = (g_otg_flash_sign > 0) ? OTG_BM_PLUS
                       : (g_otg_flash_sign < 0) ? OTG_BM_MINUS
                       :                          0;
    if (bm) {
        top_banner_render(0, bm, 16, 0, g_otg_flash_label,
                          g_otg_flash_token[0] ? g_otg_flash_token : 0);
        return;
    }
    /* No glyph: an empty bitmap keeps the label's x where every other banner
     * puts it, so the band does not jump between a confirmation and a
     * refusal. */
    static const uint16_t none[1] = { 0 };
    top_banner_render(0, none, 1, 0, g_otg_flash_label,
                      g_otg_flash_token[0] ? g_otg_flash_token : 0);
}
