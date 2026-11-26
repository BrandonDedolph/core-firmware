/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/az.c — the A-Z locator glue, the wheel's clock and the piezo
 * navigation click. Moved verbatim out of kernel/main.c.
 */

#include <stdint.h>
#include "hal.h"
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/piezo.h"
#include "../ui/text.h"
#include "../ui/chrome.h"
#include "../ui/wheel.h"        /* AZ_SHOW_TPS, wheel_tps */
#include "../ui/letterindex.h"
#include "../library/names.h"
#include "state.h"
#include "nav.h"
#include "library.h"
#include "albums.h"
#include "playlists.h"
#include "paint.h"
#include "az.h"

/* ---------------------------------------------------------------------------
 * The A-Z locator: which letter a row is under, on whichever list is up
 *
 * This used to be SONGS ONLY, for one reason: the locator's only tool was a
 * linear walk of the rows, which is affordable exactly once. ui/letterindex.c
 * turns the per-row answer into a run index built once per list, so every
 * long alphabetised list can have the plate and the letter stepping —
 * Songs, a genre's songs, an artist's All Songs, Artists, Albums (all and one
 * artist's), Playlists and Genres.
 *
 * What differs per screen is the KEY the list is sorted BY, and that is the
 * only thing main.c has to answer: Artists sort past a leading "The " (so
 * "The Kid LAROI" is under K), Albums by the album half of the folder name,
 * Songs/Playlists/Genres by the name itself. Returning 0 for a row (or for a
 * whole screen) is what keeps the queue, the tracklists, the menus and
 * Settings out of it — and it is the same 0 wheel_move()'s guard reads to
 * fall through to row scrolling.
 * ------------------------------------------------------------------------- */

/* The initial of the key row `row` is SORTED BY, or 0 for a row that is not
 * part of the order (the album list's synthetic "All Songs" row) and for
 * every screen that has no alphabetical order to locate within. */
static char screen_initial_raw(int row)
{
    switch (scr_cur()) {
    case SCR_SONGS:
        if (row >= 0 && row < g_songview_n) {
            return initial_of(g_songs[g_songview[row]].title);
        }
        return 0;
    case SCR_ARTISTS:
        if (row >= 0 && row < g_artists_n) {
            return initial_of(artist_key(g_artists[row].name));
        }
        return 0;
    case SCR_GENRES:
        if (row >= 0 && row < g_genres_n) {
            return initial_of(g_genres[row]);
        }
        return 0;
    case SCR_PLAYLISTS:
        if (row >= 0 && row < g_playlists_n) {
            return initial_of(g_playlists[row].name);
        }
        return 0;
    case SCR_BROWSER: {
        /* Depth 0 is the album list; depth 1 is one album's tracklist, which
         * is in track order and has no letters. albumlist_album_at answers -1
         * for the "All Songs" row and for a row off the end. */
        if (g_dir_depth != 0) return 0;
        int a = albumlist_album_at(row);
        return (a >= 0) ? initial_of(g_album_key[a]) : 0;
    }
    default:
        return 0;
    }
}

/* The index, and the list it was built for. g_list_epoch already bumps on
 * every content rebuild and every push/pop, so this is one compare per call
 * in the common case and a single walk when the list underneath changes. */
static letteridx_t g_letters;
static struct { int scr, depth, count; uint32_t epoch; } g_letters_for = {
    -1, -1, -1, 0
};

/* Bring the index up to date with whatever list is on screen, and hand back
 * that list's view (count 0 on a screen that is not a list). */
static void letters_ensure(list_view_t *v)
{
    v->count = 0;
    v->sel   = 0;
    (void)list_view_current(v);
    int scr = (int)scr_cur();
    if (g_letters_for.scr   == scr        && g_letters_for.depth == g_dir_depth &&
        g_letters_for.count == v->count   && g_letters_for.epoch == g_list_epoch) {
        return;
    }
    g_letters_for.scr   = scr;
    g_letters_for.depth = g_dir_depth;
    g_letters_for.count = v->count;
    g_letters_for.epoch = g_list_epoch;
    (void)letteridx_build(&g_letters, v->count, screen_initial_raw);
}

/* The ui/wheel.h seam: the locator letter for row `idx`, 0 where there is
 * none. Also the guard that decides whether this screen letter-steps at all. */
char list_initial_at(int idx)
{
    list_view_t v;
    letters_ensure(&v);
    return letteridx_letter_at(&g_letters, idx);
}

/* The letter the plate shows: the selected row's. */
char list_sel_initial(void)
{
    list_view_t v;
    letters_ensure(&v);
    return letteridx_letter_at(&g_letters, v.sel);
}

/* The ui/wheel.h letter-step seam: where one letter detent lands. `count` is
 * the wheel's view of the list and the index's own is the same number (both
 * come from list_view_current), so the index carries it. */
int list_letter_step_idx(int sel, int count, int dir)
{
    list_view_t v;
    (void)count;
    letters_ensure(&v);
    return letteridx_step(&g_letters, sel, dir);
}

/* The wheel's clock (the ui/wheel.h seam): the free-running USEC_TIMER. */
uint32_t wheel_clock(void)
{
    return mmio_read32(USEC_TIMER_ADDR);
}

/* The letter itself, on a centred plate over the flying list. Its presence is
 * exactly the signal that the wheel is stepping LETTERS rather than rows —
 * there is deliberately no state where the plate is up and the wheel is still
 * grinding through individual songs. */
void az_overlay_render(char ch)
{
    const int PW = 66, PH = 66;
    int px = (LCD_WIDTH - PW) / 2, py = (LCD_HEIGHT - PH) / 2;
    ui_round_rect(px - 1, py - 1, PW + 2, PH + 2, 13, LINEN_BORDER);
    ui_round_rect(px, py, PW, PH, 12, LINEN_PLATE);
    char s[2] = { ch, '\0' };
    int w = text_width(s, FONT_TITLE);
    ui_text(px + (PW - w) / 2, py + PH / 2 + 8, s, FONT_TITLE, LINEN_INK);

#if AZ_SHOW_TPS
    {
        char n[12];
        u32_to_dec(n, wheel_tps());
        int nw = text_width(n, FONT_SMALL);
        ui_text(px + (PW - nw) / 2, py + PH - 4, n, FONT_SMALL, LINEN_MUTED_D);
    }
#endif
}

/* Busy-wait `us` microseconds (PWM already off) — for the silent gap in the
 * multi-burst clicker profiles. Short enough not to underrun the decode buffer. */
static void ui_spin_us(uint32_t us)
{
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us) { /* spin */ }
}

/* Piezo navigation click. g_settings.clicker selects the sound profile (0 = Off;
 * 1..7 = Tick / Click / Pop / Blip / Tock / Double / Chirp). Single-tone
 * profiles are one (Hz, µs) burst; Double is two bursts with a gap; Chirp is a
 * rising three-tone sweep. Order matches settings.c CLICK_L. */
void ui_click(void)
{
    switch (g_settings.clicker) {
        case 1: piezo_click_ex(3000, 3000); break;   /* Tick  — crisp mid tick   */
        case 2: piezo_click_ex(4500, 2000); break;   /* Click — higher, shorter  */
        case 3: piezo_click_ex(1800, 5000); break;   /* Pop   — lower, fuller    */
        case 4: piezo_click_ex(6000, 1500); break;   /* Blip  — high, ultra-short*/
        case 5: piezo_click_ex(1000, 4000); break;   /* Tock  — low, woody       */
        case 6:                                       /* Double — two quick taps  */
            piezo_click_ex(4200, 1500);
            ui_spin_us(22000);
            piezo_click_ex(4200, 1500);
            break;
        case 7:                                       /* Chirp — rising sweep     */
            piezo_click_ex(3000, 1100);
            piezo_click_ex(4000, 1100);
            piezo_click_ex(5200, 1200);
            break;
        default: break;                               /* 0 = Off                  */
    }
}
