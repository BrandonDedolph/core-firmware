/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/paint.c — see paint.h.
 */

#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/ata.h"
#include "hal.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../kernel/drivepower.h"
#include "../ui/text.h"
#include "../ui/chrome.h"
#include "../ui/palette.h"
#include "../ui/sleeptimer.h"
#include "../library/names.h"
#include "state.h"
#include "layout.h"
#include "util.h"
#include "marquee.h"
#include "nav.h"
#include "library.h"
#include "battery_ui.h"
#include "statusbar.h"
#include "playlists.h"
#include "albums.h"
#include "nowplaying.h"
#include "otg.h"
#include "settings_glue.h"
#include "lists.h"
#include "search_glue.h"
#include "main_internal.h"
#include "paint.h"

/* Measured cost of the last full-frame present; defined with the present
 * throttle further down. Reported on the stats line below so the number the
 * repaint throttle has always been guessing at becomes observable. */
uint32_t g_present_cost_us;

/* ---------------------------------------------------------------------------
 * Presenting: damage-only pushes + partial list repaints
 *
 * lcd_present_fb streams all 38,400 32-bit words to the BCM with IRQs MASKED —
 * the audio DMA is single-shot and re-kicked inside its own ISR, so only the
 * 16-frame I2S FIFO (~363 us) covers a delayed interrupt. Pushing a whole frame
 * to move a selection bar one row is what forced the old fixed 150 ms repaint
 * throttle while playing. So: renderers report what they touched (console.c's
 * damage rect), we present only that, and a plain selection move repaints two
 * rows instead of clearing and redrawing the panel.
 * ------------------------------------------------------------------------- */

/* Measured cost of the last present, used to pace the next one. Seeded to a
 * full-frame-ish value so the first push is paced conservatively. */
/* Declared up with the stats line; seeded to a deliberate over-estimate so
 * the first present is throttled conservatively before it is measured. */
uint32_t g_present_cost_us = 30000u;

/*
 * Scroll diagnostics, reported on a "core: ui" UART line every 5 s in which
 * something was painted (see ui_stats_emit). These exist because "the list
 * lags" has four candidate mechanisms — the render, the present, the cover
 * reads issued under the wheel, and the playing-time throttle — and the only
 * number the log carried was the last present's cost. Every counter is a
 * store or a compare on a path that already reads the timer; none is on the
 * audio path.
 */
uint32_t g_ui_render_us;        /* CPU time of the last list paint, no present */
uint32_t g_ui_render_max_us;    /* worst paint in the window                    */
uint32_t g_ui_present_max_us;   /* worst present in the window                  */
uint32_t g_ui_full, g_ui_partial;   /* presents in the window, by path        */
uint32_t g_ui_art_reads;        /* cover loads issued (each a disk read)        */
uint32_t g_ui_art_max_us;       /* longest single pump: a spin-up lands here    */
uint32_t g_ui_art_held;         /* passes that refused to wake a parked platter */

uint32_t present_gap_us(void);  /* defined just below, with the throttle */

void ui_stats_emit(void)
{
    if (g_ui_full + g_ui_partial == 0 && g_ui_art_reads == 0 && g_ui_art_held == 0) {
        return;                            /* nothing painted: keep the log quiet */
    }
    const player_stats_t *ps = player_stats();
    uart_puts("core: ui full ");        uart_dec((int)g_ui_full);
    uart_puts(" partial ");             uart_dec((int)g_ui_partial);
    uart_puts(" render_us ");           uart_dec((int)g_ui_render_us);
    uart_puts(" max ");                 uart_dec((int)g_ui_render_max_us);
    uart_puts(" present_us ");          uart_dec((int)g_present_cost_us);
    uart_puts(" max ");                 uart_dec((int)g_ui_present_max_us);
    uart_puts(" gap_us ");              uart_dec((int)present_gap_us());
    uart_puts(" art reads ");           uart_dec((int)g_ui_art_reads);
    uart_puts(" max_us ");              uart_dec((int)g_ui_art_max_us);
    uart_puts(" held ");                uart_dec((int)g_ui_art_held);
    uart_puts(" parked ");              uart_dec(ata_is_parked());
    uart_puts(" parks ");               uart_dec((int)drivepower_parks(&g_drivepower, 0));
    uart_puts(" quick ");               uart_dec((int)drivepower_quick_parks(&g_drivepower, 0));
    uart_puts(" parks_play ");          uart_dec((int)drivepower_parks(&g_drivepower, 1));
    uart_puts(" quick ");               uart_dec((int)drivepower_quick_parks(&g_drivepower, 1));
    /* BCM health, cumulative: handshakes that ran out of budget (a post-wake
     * absorb that never retired lands here) and presents refused because the
     * panel was slept. A healthy device reads 0 0 forever. */
    uart_puts(" bcm_timeouts ");        uart_dec((int)lcd_bcm_timeouts());
    uart_puts(" refused ");             uart_dec((int)lcd_presents_refused());
    uart_puts(" decode_us_per_kframe "); uart_dec(ps ? (int)ps->decode_us_per_kframe : -1);
    uart_puts(" ring_low_pct ");        uart_dec((int)player_buf_pct());
    uart_putc('\n');
    g_ui_full = g_ui_partial = 0;
    g_ui_render_max_us = g_ui_present_max_us = 0;
    g_ui_art_reads = g_ui_art_max_us = g_ui_art_held = 0;
}

/* Present whatever has been drawn since the last console_damage_reset(), then
 * clear the damage. A full-screen damage rect (any console_clear) goes out via
 * the full-frame fast path, exactly as before. */
void ui_present_damage(void)
{
    int x, y, w, h;
    if (console_damage_get(&x, &y, &w, &h)) {
        uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
        int full = (x <= 0 && y <= 0 && w >= LCD_WIDTH && h >= LCD_HEIGHT);
        if (full) {
            lcd_present_fb(console_framebuffer());
        } else {
            lcd_present_rect(console_framebuffer(), x, y, w, h);
        }
        /* Only a FULL present sets the pace. The gap exists so the BCM can
         * retire the last frame to the panel before the next command lands
         * in its short idle budget; a 2.5 ms partial after a 10.8 ms full
         * frame said "10 ms is enough", and the transport band arriving 20 ms
         * after a full Now Playing repaint met a BCM still busy — "idle-wait
         * timed out", re-kicks, a stalled loop and 12 audio underruns (device
         * log, 2026-09-13). */
        if (full) {
            g_present_cost_us = mmio_read32(USEC_TIMER_ADDR) - t0;
        }
    }
    console_damage_reset();
}

/* How long to wait between repaints WHILE PLAYING. Self-tuning: space repaints
 * at ~4x what the last present actually cost. A cheap partial present paces
 * fast (a responsive wheel), a full-frame push still backs off to about the
 * old fixed 150 ms — but only when it really is a full frame.
 *
 * The 4x was chosen when the pixel push ran with IRQs MASKED end to end, to
 * keep that masked time under ~1/4 of the loop. lcd.c now releases the I-bit
 * between panel rows, so the DMA re-kick deadline no longer bears on this; what
 * the gap still buys is CPU for the decoder. player_pump decodes exactly one
 * 1024-frame step per pass (~23 ms of audio), so a pass that also renders and
 * presents yields less audio than it consumes, and the gap is what keeps such
 * passes the minority. Whether 4x is the right share is a question for the
 * "core: ui" line (render_us, present_us, decode_us_per_kframe, ring_low_pct):
 * on the album list every detent scrolls the window (ui_scroll_window anchors
 * the selection a third of the way down), so every scroll present is a FULL
 * frame and this multiplier is the frame rate. Do not lower it on a guess. */
uint32_t present_gap_us(void)
{
    uint32_t gap = g_present_cost_us * 4u;
    if (gap < 20000u)  gap = 20000u;      /* <=50 fps: no point going faster    */
    if (gap > 150000u) gap = 150000u;     /* the old worst-case throttle        */
    return gap;
}

/* Describe the screen on top of the stack, or return 0 if it isn't a list that
 * can be partially repainted (Settings/Charging/Now Playing render elsewhere,
 * empty lists draw a placeholder instead of rows). */
int list_view_current(list_view_t *v)
{
    v->right[0] = '\0';
    v->rh       = ROW_H;
    v->y0       = LIST_Y0;
    v->visible  = LIST_ROWS;
    switch (scr_cur()) {
    case SCR_MENU:
        g_menu_items = g_main_menu;
        g_menu_sel   = g_main_sel;
        v->title = "Core";
        v->count = main_menu_count();
        v->sel   = g_main_sel;
        v->row   = menu_row_draw;
        break;
    case SCR_MUSIC:
        g_menu_items = g_music_menu;
        g_menu_sel   = g_music_sel;
        v->title = "Music";
        v->count = MU_COUNT;
        v->sel   = g_music_sel;
        v->row   = menu_row_draw;
        break;
    case SCR_ARTISTS:
        v->title = "Artists";
        v->count = g_artists_n;
        v->sel   = g_artist_sel;
        v->row   = artists_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_SONGS:
        v->title   = g_songview_artist[0] ? g_songview_artist : "Songs";
        v->count   = g_songview_n;
        v->sel     = g_song_sel;
        v->row     = songs_row_draw;
        v->rh      = ROW_H2;
        v->visible = LIST_ROWS2;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_GENRES:
        v->title = "Genres";
        v->count = g_genres_n;
        v->sel   = g_genre_sel;
        v->row   = genres_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_QUEUE:
        v->title = "Now Playing";
        v->count = player_queue_len();
        v->sel   = g_queue_sel;
        v->row   = queue_row_draw;
        fmt_count(v->right, player_queue_current() + 1, v->count);
        break;
    case SCR_PLAYLISTS:
        /* The pinned row is always there, but a folder that is empty or would
         * not read draws its sentence UNDER it — a placeholder, not a row —
         * so those two states repaint whole. */
        if (g_playlists_err || g_playlists_n == 0) return 0;
        v->title = "Playlists";
        v->count = playlists_row_count();   /* the pinned On-The-Go row is row 0 */
        v->sel   = g_pl_sel;
        v->row   = playlists_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_PLAYLIST:
        /* Same: with no tracks the screen is a reason plus (on a saved slot)
         * the Delete row, laid out by playlist_render and not by this table. */
        if (g_pl_tracks_n == 0) return 0;
        v->title   = g_pl_open >= 0 ? g_playlists[g_pl_open].name : "Playlist";
        v->count   = playlist_row_count();  /* + the saved slot's Delete row */
        v->sel     = g_plt_sel;
        v->row     = playlist_row_draw;
        v->rh      = ROW_H2;
        v->visible = LIST_ROWS2;
        if (v->sel < g_pl_tracks_n) {
            fmt_count(v->right, v->sel + 1, g_pl_tracks_n);
        }
        break;
    case SCR_OTG:
        /* An empty list draws only its two sentences — the greyed action rows
         * are not drawn at all, because neither of them does anything — so it
         * is not a partially-repaintable list. */
        if (g_otg.n == 0) return 0;
        v->title   = "On-The-Go";
        v->count   = otg_row_count();
        v->sel     = g_otg_sel;
        v->row     = otg_row_draw;
        v->rh      = ROW_H2;
        v->visible = LIST_ROWS2;
        otg_header_right(v->right, v->sel);
        break;
    case SCR_BROWSER:
        if (g_dir_depth == 0) {
            v->title   = g_artist_filter[0] ? g_artist_filter : "Albums";
            v->count   = albumlist_count();
            v->sel     = g_br_sel;
            v->row     = albumlist_row_draw;
            v->rh      = ROW_H2;
            v->visible = LIST_ROWS2;
            fmt_count(v->right, v->sel + 1, v->count);
        } else {
            /* The tracklist scrolls over the DISPLAY view (tracks interleaved
             * with "Disc N" headers), so the row space is view indices. */
            v->title   = "Albums";
            v->count   = g_det_view_n;
            v->sel     = detail_sel_view(g_det_sel);
            v->row     = detail_row_draw;
            v->y0      = DET_LIST_Y0;
            v->visible = DET_ROWS;
            fmt_count(v->right, g_det_sel + 1, g_browse_n > 0 ? g_browse_n : 1);
        }
        break;
    default:
        return 0;
    }
    return v->count > 0;
}

list_paint_t g_lp;

static uint32_t chrome_key(void)
{
    uint32_t k = player_active() ? 1u : 0u;
    k = k * 31u + (player_paused() ? 1u : 0u);
    k = k * 31u + (uint32_t)(player_queue_current() + 1);
    k = k * 31u + (uint32_t)(g_locked ? 1 : 0);
    k = k * 31u + (uint32_t)(g_bat_pct + 1);
    /* The strip's SLEEP token, for the same reason the battery percentage is
     * here: a partial two-row repaint under a changed minute would leave a
     * stale token on the panel. */
    k = k * 31u + (uint32_t)sleeptimer_remaining_min(&g_sleep);
    return k;
}

/* Record what we just painted (called after every paint of a list screen). */
void list_paint_note(void)
{
    list_view_t v;
    if (!list_view_current(&v)) {
        g_lp.valid = 0;
        return;
    }
    g_lp.valid   = 1;
    g_lp.scr     = (int)scr_cur();
    g_lp.depth   = g_dir_depth;
    g_lp.sel     = v.sel;
    g_lp.count   = v.count;
    g_lp.top     = ui_scroll_window(v.sel, v.count, v.visible);
    g_lp.right_w = v.right[0] ? text_width(v.right, FONT_SMALL) : 0;
    g_lp.chrome  = chrome_key();
    g_lp.epoch   = g_list_epoch;
    g_lp.rows    = 0;                     /* a full paint drew every row       */
}

/* Repaint the current screen by redrawing ONLY what changed under a still
 * view: the two rows a selection move touched (plus the header's "n / m" value
 * and the scrollbar), and any row whose cover chip arrived since the last
 * paint (g_lp.rows). Returns 0 when that isn't provably sufficient (different
 * screen, scrolled window, changed contents or chrome) — the caller then does
 * the full render.
 *
 * The chip case used to fall through to the full render: with the selection
 * unchanged there was "nothing" for the partial path to do, so each cover that
 * landed on the album list cleared and re-pushed the whole panel — six times
 * over for a fresh window, each one re-arming the playing-time throttle at the
 * full-frame cost, in the middle of the scroll the covers were loading for. */
int list_repaint_partial(void)
{
    list_view_t v;
    if (!g_lp.valid || !list_view_current(&v)) return 0;
    if (g_lp.scr != (int)scr_cur() || g_lp.depth != g_dir_depth) return 0;
    if (g_lp.epoch != g_list_epoch)                              return 0;
    if (g_lp.count != v.count || g_lp.chrome != chrome_key())    return 0;
    int moved = (v.sel != g_lp.sel);
    if (!moved && g_lp.rows == 0)                                return 0;
    int top = ui_scroll_window(v.sel, v.count, v.visible);
    if (top != g_lp.top) return 0;        /* window scrolled: every row moved   */

    uint32_t rows = g_lp.rows;
    if (moved) {
        int r_old = g_lp.sel - top, r_new = v.sel - top;
        if (r_old < 0 || r_old >= v.visible || r_new < 0 || r_new >= v.visible) {
            return 0;                      /* off-window selection: play it safe */
        }

        /* Header value: clear only as wide as the old/new text. If the title
         * reaches into that box, the clear would eat it — fall back to a full
         * paint. */
        int rw = v.right[0] ? text_width(v.right, FONT_SMALL) : 0;
        int cw = ((rw > g_lp.right_w) ? rw : g_lp.right_w) + 4;
        if (cw > 4) {
            int cx = LCD_WIDTH - 12 - cw;
            if (12 + 20 + text_width(v.title, FONT_HEADER) > cx) return 0;
            console_fill_rect(cx, HDR_BASE - 12, cw, 16, LINEN_SURFACE);
            if (rw) {
                ui_text(LCD_WIDTH - 12 - rw, HDR_BASE - 1, v.right, FONT_SMALL,
                        LINEN_MUTED2);
            }
        }
        rows |= (1u << r_old) | (1u << r_new);
    }

    /* The rows: clear each band (the new content may be narrower) + redraw.
     * The clear stops short of the scrollbar column (ui_list_row_clear). A
     * full-width clear here, with the bar only redrawn on a MOVE below, is how
     * the album list's scrollbar vanished under a fast scroll: the covers
     * landing on the settled window repainted every row band edge to edge and
     * took the bar with them, slice by slice, until the next full paint. */
    for (int r = 0; r < v.visible; r++) {
        if (!(rows & (1u << r))) continue;
        ui_list_row_clear(v.y0, r, v.rh);
        int idx = top + r;
        if (idx < v.count) v.row(r, idx);
    }
    /* Unconditional, not just on `moved`: every partial present that touches a
     * row band carries the bar with it, so no row repaint can leave the panel
     * without one. Two small fills — nothing to save by skipping them. (A
     * no-op when everything fits, exactly as in the full render.) */
    ui_scrollbar(v.y0, top, v.visible, v.count);
    g_lp.rows = 0;
    return 1;
}

/* Render whatever screen is on top of the stack into the framebuffer (no
 * present) — used to paint context behind the Hold banner. */
void paint_current_screen(void)
{
    g_mq.active = 0;                       /* a fresh paint re-registers any marquee */
    switch (scr_cur()) {
    case SCR_MENU:    main_menu_render();  break;
    case SCR_MUSIC:   music_menu_render(); break;
    case SCR_ARTISTS: artists_render(g_artist_sel); break;
    case SCR_SONGS:   songs_render(g_song_sel);     break;
    case SCR_GENRES:  genres_render(g_genre_sel);   break;
    case SCR_PLAYLISTS: playlists_render(g_pl_sel); break;
    case SCR_PLAYLIST:  playlist_render(g_plt_sel); break;
    case SCR_OTG:       otg_render(g_otg_sel);      break;
    case SCR_SEARCH:    search_render_cur(); break;
    case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel); break;
    case SCR_QUEUE:   queue_render(g_queue_sel); break;
    case SCR_SETTINGS: settings_render_cur(); break;
    case SCR_NOWPLAYING:
        nowplaying_render(player_track_name(), player_elapsed_s(),
                          player_total_s(), player_buf_pct());
        break;
    case SCR_BATTERY:
        battery_modal_paint();
        break;
    case SCR_CHARGING:
        charging_screen_paint();
        break;
    }
}
