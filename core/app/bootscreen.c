/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/bootscreen.c — see bootscreen.h.
 */

#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/lcd.h"
#include "hal.h"
#include "../kernel/console.h"
#include "core_version.h"       /* CORE_BUILD_ID: meson vcs_tag, git describe   */
#include "../ui/text.h"
#include "../ui/chrome.h"
#include "bootscreen.h"

/* ---------------------------------------------------------------------------
 * Linen theme + Nunito faces
 * ------------------------------------------------------------------------- */

/* Theme tokens (LINEN_*) and the Nunito face macros moved to ui/chrome.h,
 * next to the list chrome that consumes them — and so has the anti-aliased
 * rounded rect that used to live here as fill_round_rect_aa: it is
 * ui_round_rect now, the one rounded rect for bars and plates alike. */
#define AA_SS 16                            /* S*S sub-samples, S = 4          */
/* Anti-aliased filled disc of radius r, centred on the pixel EDGE (cx, cy) —
 * it covers pixels cx-r .. cx+r-1, the same convention as
 * ui_round_rect(cx-r, cy-r, 2r, 2r, r). The boot mark's ring, dot and
 * hole. ui_round_rect cannot draw it: its memoized corner mask stops at
 * UI_RR_MAX_R (16) and the ring is 19, and sizing that table for one caller
 * would grow it for every plate. Same 4x4 super-sampling and the same gamma
 * blend as the plates, boundary pixels only; pixels wholly inside are solid, wholly
 * outside untouched. Reads the FB, so paint the background first, and paint
 * the discs largest first (the ring is an INK disc with a SURFACE disc inside
 * it). Drawn a few times per second at most (the boot screen), so the
 * per-pixel sub-sampling is fine. */
static void fill_disc_aa(int cx, int cy, int r, uint16_t c)
{
    uint16_t *fb = console_fb();
    int R2 = (8 * r) * (8 * r);
    /* Blended straight into the framebuffer, so the damage tracker would never
     * see it: report the bounding box once, as ui_round_rect does. */
    console_damage_add(cx - r - 1, cy - r - 1, 2 * r + 2, 2 * r + 2);
    for (int py = cy - r - 1; py <= cy + r; py++) {
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int px = cx - r - 1; px <= cx + r; px++) {
            if (px < 0 || px >= LCD_WIDTH) continue;
            int inside = 0;
            for (int j = 0; j < 4; j++) {
                int dy = 8 * (py - cy) + 2 * j + 1;
                for (int i = 0; i < 4; i++) {
                    int dx = 8 * (px - cx) + 2 * i + 1;
                    if (dx * dx + dy * dy <= R2) inside++;
                }
            }
            if (inside == 0) continue;
            uint16_t *d = &fb[py * LCD_WIDTH + px];
            *d = (inside >= AA_SS) ? c
                : text_blend565(*d, c, (uint8_t)((inside * 255 + AA_SS / 2) / AA_SS));
        }
    }
}

/* ---------------------------------------------------------------------------
 * Boot screen — the one screen from power-on to the menu.
 *
 * The design reference's boot splash (system-screens.jsx BootSplash), made
 * Core's and stacked: the click-wheel mark, "Core" under it, the device line
 * under that, and along the bottom a 2 px ink bar with the phase in small caps
 * beneath it. main() paints it with no bar the moment the panel is ours
 * ("LOADING"), and the library load repaints it with the bar as the phases
 * advance. One painter, so the boot never changes voice halfway through.
 *
 * THEME: everything here is drawn from g_pal, so once theme_set has run the
 * screen is the user's theme. The FIRST paint, before the disk is mounted, is
 * necessarily the default (Linen) palette: the saved theme lives in the
 * settings record on the music partition, and reading anything from the disk
 * is the spin-up this screen exists to cover. run_ui repaints it in the theme
 * as soon as the settings are read, BEFORE the library load — one early flip
 * for a dark-theme user, then a themed bar (see the setup order there).
 *
 * `phase` is the small-caps line under the bar; pct < 0 draws no bar.
 * ------------------------------------------------------------------------- */
#define BOOT_MARK_CY   86
#define BOOT_BAR_Y     (LCD_HEIGHT - 34)
#define BOOT_BAR_X     60
#define BOOT_BAR_W     (LCD_WIDTH - 120)

void boot_screen_render(const char *phase, int pct)
{
    console_clear(LINEN_SURFACE);
    int cx = LCD_WIDTH / 2, cy = BOOT_MARK_CY;
    fill_disc_aa(cx, cy, 19, LINEN_INK);       /* ring: ink disc ...       */
    fill_disc_aa(cx, cy, 17, LINEN_SURFACE);   /* ... with a surface disc  */
    fill_disc_aa(cx, cy,  6, LINEN_INK);       /* centre dot               */
    fill_disc_aa(cx, cy,  2, LINEN_SURFACE);   /* its hole                 */
    ui_text_centered(cy + 19 + 30, "Core", FONT_TITLE, LINEN_INK);
    ui_text_centered(cy + 19 + 46, "IPOD VIDEO  " UI_GLYPH_MIDDOT "  5.5 GEN",
                     FONT_SMALL, LINEN_MUTED2);
    if (pct >= 0) {
        if (pct > 100) pct = 100;
        console_fill_rect(BOOT_BAR_X, BOOT_BAR_Y, BOOT_BAR_W, 2, LINEN_TRK);
        int fw = BOOT_BAR_W * pct / 100;
        if (fw > 0) console_fill_rect(BOOT_BAR_X, BOOT_BAR_Y, fw, 2, LINEN_INK);
    }
    ui_text_centered(LCD_HEIGHT - 16, phase, FONT_SMALL, LINEN_MUTED2);
    /* Build stamp, bottom right, in the border colour: readable if you look,
     * invisible if you don't. `git describe --always --dirty --abbrev=7` at
     * build time (core/meson.build vcs_tag) — so an image flashed off an
     * uncommitted tree says "-dirty" on its own boot screen. */
    int vw = text_width(CORE_BUILD_ID, FONT_SMALL);
    ui_text(LCD_WIDTH - 8 - vw, LCD_HEIGHT - 4, CORE_BUILD_ID, FONT_SMALL,
            LINEN_BORDER);
    lcd_present_fb(console_framebuffer());
}

/* The boot screen with its progress bar (0..100%), repainted from the library
 * load phases so a multi-second first load shows real progress instead of a
 * frozen splash. Same screen main() already put up — only the bar and the
 * phase line change. */
/*
 * Rate-limited so callers can call it as often as they like.
 *
 * Each call is a console_clear (76,800 pixel writes) plus a full 153,600-byte
 * lcd_present_fb with IRQs masked. Callers sample on an iteration count, which
 * scales with the library rather than with what an eye can follow: the boot
 * resolve pass paints every 4th album, so a 256-album library paid 64 full-frame
 * repaints — a large slice of "Loading Library" was the bar drawing itself.
 *
 * A progress bar needs a handful of updates per second, so drop anything inside
 * LOAD_BAR_MIN_GAP_US of the last paint. 0% and 100% always paint, so the screen
 * still appears immediately and always finishes filled.
 */
#define LOAD_BAR_MIN_GAP_US 120000u

static uint32_t g_load_bar_last_us;
static int      g_load_bar_last_pct = -1;

void load_bar(const char *phase, int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    if (pct != 0 && pct != 100) {
        uint32_t now = mmio_read32(USEC_TIMER_ADDR);
        if (pct == g_load_bar_last_pct ||
            (uint32_t)(now - g_load_bar_last_us) < LOAD_BAR_MIN_GAP_US) {
            return;
        }
    }
    g_load_bar_last_us  = mmio_read32(USEC_TIMER_ADDR);
    g_load_bar_last_pct = pct;

    boot_screen_render(phase, pct);
}

/*
 * Deferred progress bar.
 *
 * load_bar() is expensive: console_clear() is 76,800 pixel writes and
 * lcd_present_fb() streams the whole 153,600-byte frame with IRQs masked. The
 * queue builders called it every 64 songs, so picking a track out of Songs paid
 * ~19 full-frame repaints — and since the queue build is now O(n) with a hash
 * lookup and a struct copy per entry, the BAR was most of the wait. Reporting
 * progress cost more than the work it was reporting on.
 *
 * So: stay silent until the job has actually run long enough to need feedback,
 * then behave normally. A library that enqueues in 30 ms shows nothing; one slow
 * enough to look hung still gets a bar.
 */
#define LOAD_BAR_DELAY_US 250000u

static uint32_t g_load_bar_t0;
static int      g_load_bar_shown;

void load_bar_begin(void)
{
    g_load_bar_t0    = mmio_read32(USEC_TIMER_ADDR);
    g_load_bar_shown = 0;
}

void load_bar_progress(const char *phase, int pct)
{
    if (!g_load_bar_shown) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - g_load_bar_t0)
            < LOAD_BAR_DELAY_US) {
            return;
        }
        g_load_bar_shown = 1;                 /* crossed the threshold: show it */
    }
    load_bar(phase, pct);
}
