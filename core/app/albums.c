/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/albums.c — see albums.h.
 */

#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/lcd.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../ui/text.h"
#include "../ui/thumb.h"
#include "../ui/artcache.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "../library/sort.h"
#include "layout.h"
#include "marquee.h"
#include "nav.h"
#include "library.h"
#include "statusbar.h"
#include "../ui/wheel.h"
#include "hw/ata.h"
#include "paint.h"
#include "main_internal.h"
#include "albums.h"
#include "coverpump.h"

/* ---------------------------------------------------------------------------
 * File / album browser
 *
 * browse_entry_t + BROWSE_MAX/NAME_MAX are shared with the player (player.h):
 * a folder's worth of entries is copied into the player as its queue.
 * ------------------------------------------------------------------------- */

browse_entry_t g_browse[BROWSE_MAX];
int            g_browse_n;

/* Browser navigation. The album LIST (depth 0) is index-driven — see g_albums
 * below — while the TRACKLIST (depth 1) is a live folder read into g_browse.
 * Albums are flat on disk (import flattens multi-disc), so the browser is only
 * ever two levels: g_dir_depth is 0 (albums) or 1 (an album's tracks).
 * g_cur_dir is the cluster of the album whose tracks are currently listed. */
uint32_t g_cur_dir;
int      g_dir_depth;

/* Browser view state (was local to the old browse loop). The album LIST (depth
 * 0) and a single album's TRACKLIST (depth 1) keep SEPARATE selections, so
 * backing out of an album returns the cursor to that album in the list rather
 * than jumping to the top. */
int g_br_sel, g_br_accum;      /* album list (depth 0)  */
int g_chip_last_sel, g_chip_dir = 1;  /* cover look-ahead's direction */
/* This tracklist was opened straight from a Search hit, so the album list
 * underneath it was never on screen: MENU must go back to the results, not
 * reveal a list the user did not ask for. Cleared by every other push. */
int g_br_from_search;

int g_det_sel, g_det_accum;    /* tracklist   (depth 1) */

/* Album art (folder.art) location captured while enumerating the current
 * folder; handed to player_play_queue so the player owns/validates it. */
uint32_t g_art_clus, g_art_size;

/* When set, the album list (depth 0) only shows folders whose "Artist - Album"
 * name has this artist prefix — the Artists → one-artist's-albums drill-down.
 * Empty means the plain "Albums" list (every folder). */
char g_artist_filter[NAME_MAX + 1];

/* fat32_readdir callback: collect music subdirectories + playable files into
 * g_browse. Directories named like iPod/OS system folders (or dotfolders) are
 * junk-filtered out; non-playable files are skipped. */
static int browse_collect(void *ud, const fat32_dirent_t *e)
{
    (void)ud;

    /* Capture the folder's album-art sidecar (not shown as a list row). */
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_art_clus = e->first_clus;
        g_art_size = e->size;
        return 0;
    }

    if (g_browse_n >= BROWSE_MAX) return 1;   /* array full: stop enumeration */

    if (e->is_dir) {
        if (is_junk_dir(e->name)) return 0;       /* skip system/Apple folders */
        /* The browser is exactly two levels (albums, then one album's tracks —
         * import flattens multi-disc), so a folder nested INSIDE an album
         * ("Scans", "Artwork") is never navigable. Listing it anyway put a dead
         * entry in g_browse: the wheel ranged over g_browse_n (dirs included)
         * while the tracklist view skipped dirs, so one wheel position selected
         * nothing visible and SELECT there played a different track. */
        if (g_dir_depth != 0) return 0;
        /* Artists drill-down: at the album-list level, keep only this artist's
         * folders (name "Artist - Album"). */
        if (g_dir_depth == 0 && g_artist_filter[0]) {
            char a[NAME_MAX + 1], b2[NAME_MAX + 1];
            split_artist_album(e->name, a, b2);
            if (!name_eq_ci(a, g_artist_filter)) return 0;
        }
        browse_entry_t *b = &g_browse[g_browse_n++];
        copy_display_name(b->name, e->name, 0);   /* keep folder name as-is */
        b->clus   = e->first_clus;
        b->size   = 0;
        b->fmt    = 0;
        b->is_dir = 1;
        /* A row is never memset, so every field has to be written or it keeps
         * whatever the previous listing left there. A folder never plays, so
         * it belongs to no album and has no place in one. */
        b->album     = 0;
        b->order_key = 0xFFFFFFFFu;
        return 0;
    }

    /* The album/artist lists (depth 0) show only folders — hide loose files at
     * the root (leftover TEST.*, stray downloads) so they don't clutter it. */
    if (g_dir_depth == 0) return 0;

    int fmt = classify_ext(e->name);
    if (fmt < 0) return 0;
    browse_entry_t *b = &g_browse[g_browse_n++];
    copy_display_name(b->name, e->name, 1);       /* trim ".flac" -> title */
    b->clus   = e->first_clus;
    b->size   = e->size;
    b->fmt    = (uint8_t)fmt;
    b->is_dir = 0;
    b->art_clus = b->art_size = 0;                 /* album play: queue-level art */
    /* One folder is one album, so every row here shares a group id. The place
     * within it is the index's and browse_bind() writes it; "unbound" is the
     * right value until then, and a listing whose directory read failed never
     * gets that far. */
    b->album     = 1;
    b->order_key = 0xFFFFFFFFu;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Album detail view (collection-detail.jsx AlbumDetail): a 56x56 art hero with
 * title + track count, then the folder's tracklist. Shown when you enter an
 * album folder (browser depth > 0). The hero art is the entered folder's
 * folder.art, downscaled on-device to 56x56 (thumb_downscale_rgb565).
 * ------------------------------------------------------------------------- */
#define DET_HERO_Y   42                   /* art hero top                         */
#define DET_ART      56                   /* hero art dimension                   */

/* The raw folder.art staging buffer is the ART CACHE's — see artcache_scratch().
 * There used to be a second, byte-identical ART_RAW_MAX array here: 28,812 bytes
 * duplicated for no reason, since the cache's copy is only live inside
 * artcache_pump() and this path (entering an album) never pumps. */
#define ART_RAW_MAX  ARTCACHE_SCRATCH_SZ
static uint16_t g_detail_art[DET_ART * DET_ART]; /* downscaled 56x56 hero           */
static int      g_detail_art_ok;
char     g_album_title[NAME_MAX + 1];     /* album part of "Artist - Album" */
char     g_album_artist[NAME_MAX + 1];    /* artist part (empty if none)    */
static int      g_album_track_n;                  /* playable files in the folder   */

/* Per-track disc/duration (from the index) + a display "view" that interleaves
 * "Disc N" header rows for multi-disc albums. Filled in detail_load_meta. A view
 * entry >= 0 is a track index into g_browse; < 0 encodes a header disc as
 * -(disc+1). */
static uint16_t g_track_dur[BROWSE_MAX];
static uint16_t g_track_num[BROWSE_MAX];
static uint8_t  g_track_disc[BROWSE_MAX];
/* Real (tag) title per browse row, bound from the index in detail_load_meta.
 * The on-disk filename is FAT-sanitized (?,*,:,/ -> _), so the tracklist shows
 * this instead when available; NULL falls back to the filename. */
static const char *g_track_title[BROWSE_MAX];
int16_t  g_det_view[DET_VIEW_MAX];
int      g_det_view_n;
static int      g_detail_multidisc;
/* Total runtime of the folder's tracks, summed ONCE in detail_load_meta (the
 * meta line used to re-sum every track on every paint). 0 = durations unknown. */
static uint32_t g_detail_total_s;

/* Read the current folder's folder.art (clus/size captured by browse_load) and
 * downscale it to the 56x56 hero. Leaves g_detail_art_ok=0 if absent/malformed.
 * Same CoreArt "CART" validation the player uses (player.c load_folder_art). */
static void detail_art_load(fat32_t *fs)
{
    g_detail_art_ok = 0;
    if (g_art_clus == 0 || g_art_size < 12 || g_art_size > (uint32_t)ART_RAW_MAX) {
        return;
    }
    uint8_t *raw = artcache_scratch();
    int32_t  n   = fat32_read_file(fs, g_art_clus, raw, g_art_size);
    if (n < 12) {
        return;
    }
    if (raw[0] != 'C' || raw[1] != 'A' || raw[2] != 'R' || raw[3] != 'T') {
        return;
    }
    int w = raw[6] | (raw[7] << 8);
    int h = raw[8] | (raw[9] << 8);
    if (w <= 0 || h <= 0 || w > 120 || h > 120) {
        return;
    }
    if ((int32_t)(12 + w * h * 2) > n) {
        return;
    }
    thumb_box_rgb565((const uint16_t *)(raw + 12), w, h,
                     g_detail_art, DET_ART, DET_ART);
    g_detail_art_ok = 1;
}

/* Animated three-bar "now playing" glyph (collection-detail.jsx NowPlayingDot).
 * Each bar bounces on a triangle wave with its own phase; bottom-anchored at
 * y+9, heights 3..9. `t_us` is the free-running microsecond clock. */

static int nowplaying_bar_h(uint32_t t_ms, int phase)
{
    uint32_t p = (t_ms + (uint32_t)phase) % 760u;      /* period 760 ms          */
    int tri = (p < 380u) ? (int)p : (int)(760u - p);   /* 0..380..0 triangle     */
    return 3 + (tri * 6) / 380;                          /* 3..9 px               */
}

/* The bars' own clock: the USEC_TIMER less every pause so far. While the
 * player is paused it stands still, so the bars freeze mid-bounce and carry
 * on from the same shape when the music does — the owner's rule, "the bars
 * should only be moving if music is actually playing", which the desktop app
 * follows too (its NowPlaying.BarsPausedAt). Before this they were keyed on
 * player_active(), which stays 1 across a pause, so a paused track's bars
 * kept bouncing. Self-contained: every caller reads the time through here. */
uint32_t bars_clock(void)
{
    static uint32_t hold_us, pause_at;
    static int      was_paused;
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    int paused = player_paused();
    if (paused) {
        if (!was_paused) pause_at = now;
        was_paused = 1;
        return pause_at - hold_us;
    }
    if (was_paused) {
        hold_us += now - pause_at;
        was_paused = 0;
    }
    return now - hold_us;
}

void nowplaying_bars(int x, int y, uint16_t c, uint32_t t_us)
{
    uint32_t ms = t_us / 1000u;
    static const int ph[3] = { 0, 250, 500 };
    for (int i = 0; i < 3; i++) {
        int h = nowplaying_bar_h(ms, ph[i]);
        console_fill_rect(x + i * 3, y + (NP_BARS_H - h), 2, h, c);
    }
}

/* One tracklist row of the album detail, at list-row `r` showing view entry
 * `vi`. Split out of detail_render so a selection move can repaint just the two
 * rows that changed instead of the whole panel (see list_repaint_partial). */
void detail_row_draw(int r, int vi)
{
    int ry = DET_LIST_Y0 + r * ROW_H;
    int16_t v = g_det_view[vi];
    /* Single-disc albums have no "DISC N" header rows, so the per-disc number
     * gutter is wasted width — pull the numbers and titles left to reclaim it. */
    int num_rx  = g_detail_multidisc ? 30 : 24;   /* track-number right-align x */
    int title_x = g_detail_multidisc ? 38 : 30;   /* title left x               */
    if (v < 0) {                          /* a "Disc N" section header */
        char h[12];
        int hi = 0;
        for (const char *p = "DISC "; *p; p++) h[hi++] = *p;
        u32_to_dec(h + hi, (unsigned)(-(int)v - 1));
        ui_text(14, UI_ROW_BASE(ry, ROW_H), h, FONT_SMALL, LINEN_MUTED_D);
        return;
    }
    int idx = v;
    const browse_entry_t *e = &g_browse[idx];
    int is_sel = (idx == g_det_sel);
    if (is_sel) {
        ui_round_rect(6, ry + 1, LCD_WIDTH - 16, ROW_H - 2, 4, LINEN_SEL_BG);
    }
    uint16_t fg = is_sel ? LINEN_SEL_FG : LINEN_INK;
    uint16_t nc = is_sel ? LINEN_SEL_SUB : LINEN_MUTED2;
    /* Left gutter: the now-playing bars for the playing track, else its
     * (per-disc) track number (collection-detail.jsx). */
    const char *playing = player_active() ? player_track_name() : 0;
    if (playing && name_eq_ci(e->name, playing)) {
        nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK,
                        bars_clock());
    } else {
        char num[6];
        u32_to_dec(num, (unsigned)(g_track_num[idx] ? g_track_num[idx]
                                                    : idx + 1));
        int nw = text_width(num, FONT_SMALL);
        /* Regular-9 digits are 7 px, the title's caps 10: on the title's
         * baseline the number hung 1.5 px low. Centre its own cap band. */
        ui_text(num_rx - nw, UI_ROW_BASE_FOR(ry, ROW_H, FONT_SMALL), num,
                FONT_SMALL, nc);
    }
    /* Duration on the right (from the index); the title must stop before it. */
    int title_right = LCD_WIDTH - 16;
    if (g_track_dur[idx]) {
        char dts[FMT_TIME_MAX];
        fmt_time(dts, g_track_dur[idx]);
        int dw = text_width(dts, text_font_bold_12());
        /* Bold-12 figures (8 px) centred on the row in their own right, the
         * way ui_list_row places every right-hand value. */
        ui_text(LCD_WIDTH - 16 - dw, UI_ROW_BASE_FOR(ry, ROW_H, text_font_bold_12()),
                dts, text_font_bold_12(), is_sel ? LINEN_SEL_SUB : LINEN_MUTED_D);
        title_right = LCD_WIDTH - 16 - dw - 8;
    }
    /* Title (clean — number gutter provides the index), indented past it,
     * CLIPPED before the duration so a long title can't overlap it; the
     * selected row's long title scrolls (marquee). */
    const text_font_t *tf = is_sel ? FONT_HEADER : FONT_ROW;
    /* Prefer the real tag title (bound from the index): the filename is
     * FAT-sanitized (e.g. "WHO CARES?" -> "WHO CARES_"), the tag isn't. */
    const char *tt = g_track_title[idx] ? g_track_title[idx]
                                        : track_display(e->name);
    if (is_sel) {
        mq_text(title_x, UI_ROW_BASE(ry, ROW_H), title_right - title_x, tt, tf, fg,
                LINEN_SEL_BG, ry, ry + ROW_H);
    } else {
        ui_text_clip(title_x, UI_ROW_BASE(ry, ROW_H), tt, tf, fg, title_x, title_right);
    }
}

/* The selected track's position in the display view (which interleaves the
 * "Disc N" header rows), i.e. the row space the scroll window works in. */
int detail_sel_view(int sel)
{
    for (int i = 0; i < g_det_view_n; i++) {
        if (g_det_view[i] == (int16_t)sel) return i;
    }
    return 0;
}

static void detail_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    fmt_count(right, sel + 1, g_browse_n > 0 ? g_browse_n : 1);
    ui_header("Albums", right, 1);

    /* Hero art (or a placeholder tile when the folder has no folder.art). */
    if (g_detail_art_ok) {
        console_blit565(12, DET_HERO_Y, DET_ART, DET_ART, g_detail_art);
    } else {
        console_fill_rect(12, DET_HERO_Y, DET_ART, DET_ART, LINEN_BORDER);
    }
    int tx = 12 + DET_ART + 12;
    ui_text(tx, DET_HERO_Y + 15, g_album_title, FONT_HEADER, LINEN_INK);
    if (g_album_artist[0]) {
        ui_text(tx, DET_HERO_Y + 31, g_album_artist, FONT_SUB, LINEN_MUTED_D);
    }
    char meta[40];
    int mi = u32_to_dec(meta, (unsigned)g_album_track_n);
    for (const char *p = (g_album_track_n == 1) ? " track" : " tracks"; *p; p++)
        meta[mi++] = *p;
    /* Append total runtime ("11 tracks · 1h 41m") when durations are known
     * (index path). Summed ONCE in detail_load_meta — this runs on every paint.
     * collection-detail.jsx meta line. */
    if (g_detail_total_s > 0) {
        for (const char *p = " " UI_GLYPH_MIDDOT " "; *p; p++) meta[mi++] = *p;
        uint32_t tot_m = (g_detail_total_s + 59u) / 60u;  /* round UP to minutes */
        uint32_t hh = tot_m / 60u, mm = tot_m % 60u;
        if (hh > 0) {
            mi += u32_to_dec(meta + mi, hh);
            meta[mi++] = 'h'; meta[mi++] = ' ';
        }
        mi += u32_to_dec(meta + mi, mm);
        meta[mi++] = 'm';
    }
    meta[mi] = '\0';
    ui_text(tx, DET_HERO_Y + 47, meta, FONT_SMALL, LINEN_MUTED2);

    console_fill_rect(12, DET_LIST_Y0 - 6, LCD_WIDTH - 24, 1, LINEN_BORDER);

    if (g_browse_n == 0) {
        ui_text(14, DET_LIST_Y0 + 14, "Empty folder", FONT_ROW, LINEN_MUTED);
        return;
    }
    /* Scroll over the display view (tracks + any "Disc N" headers), centered on
     * the selected track's position within it. */
    int top = ui_scroll_window(detail_sel_view(sel), g_det_view_n, DET_ROWS);
    for (int r = 0; r < DET_ROWS; r++) {
        int vi = top + r;
        if (vi >= g_det_view_n) break;
        detail_row_draw(r, vi);
    }
    ui_scrollbar(DET_LIST_Y0, top, DET_ROWS, g_det_view_n);
}

/* A neutral tile shown in a row's chip slot until its real cover loads, so the
 * text doesn't shift right when the thumbnail pops in. Filled once at startup. */
static uint16_t g_chip_ph[ARTCACHE_DIM * ARTCACHE_DIM];
/* Diagnostic placeholders — see albumlist_row_draw. */
static uint16_t g_chip_noclus[ARTCACHE_DIM * ARTCACHE_DIM];
static uint16_t g_chip_noart[ARTCACHE_DIM * ARTCACHE_DIM];

void chip_placeholder_init(void)
{
    for (int i = 0; i < ARTCACHE_DIM * ARTCACHE_DIM; i++) {
        g_chip_ph[i]     = LINEN_BORDER;
        g_chip_noclus[i] = 0xFD20;             /* amber: no cluster resolved   */
        g_chip_noart[i]  = 0xF800;             /* red:   load gave up          */
    }
}

/* Queue every album row's cover into the incremental thumbnail cache. Called
 * when the album list is (re)entered; artcache_pump loads them a few frames
 * apart off the audio path. NOT reset here — artcache_queue is idempotent (a
 * slot whose cover is unchanged keeps its loaded pixels), so backing out of an
 * album detail re-uses the covers already loaded instead of reloading them all.
 * A changed view (e.g. a different artist filter) re-queues only the slots whose
 * album actually changed. */
void albumlist_queue_chips(void)
{
    for (int i = 0; i < g_albumview_n; i++) {
        int a = g_albumview[i];
        if (a >= ARTCACHE_SLOTS) continue;
        const lib_album_t *al = &g_albums[a];
        artcache_queue(a, al->thm_clus, al->thm_size, al->art_clus, al->art_size);
    }
}

/*
 * The album list carries a synthetic "All Songs" row at index 0 WHEN it is
 * showing a single artist — browsing an artist by album alone makes a track you
 * remember but cannot place hard to reach. Row 0 is the artist's whole
 * discography in title order; rows 1.. are the albums.
 *
 * Everything that indexes the list has to agree about that offset, so it is
 * expressed once here and every caller goes through albumlist_album_at().
 */
static int albumlist_all_row(void)
{
    return g_artist_filter[0] ? 1 : 0;
}

int albumlist_count(void)
{
    return g_albumview_n + albumlist_all_row();
}

/* Global g_albums[] index for a list row, or -1 for the "All Songs" row. */
int albumlist_album_at(int row)
{
    int k = row - albumlist_all_row();
    if (k < 0 || k >= g_albumview_n) {
        return -1;
    }
    return g_albumview[k];
}

/* One album row at list-row `r` showing album-view entry `idx`. Split out of
 * albumlist_render so a selection move can repaint just the rows that changed. */
void albumlist_row_draw(int r, int idx)
{
    int a = albumlist_album_at(idx);
    if (a < 0) {                            /* the synthetic "All Songs" row */
        list_row_tall(r, "All Songs", 0, 0, 1, idx == g_br_sel, 0, 0);
        return;
    }
    const lib_album_t *e = &g_albums[a];
    /* (Re)queue the visible rows every paint — idempotent for a slot that
     * already holds this album, and it's what gets a cover for rows past the
     * bulk prefetch in albumlist_queue_chips when the library has more albums
     * than the cache has slots.
     *
     * Keyed by the GLOBAL album index, not the row: the row number shifts when
     * an artist filter adds the "All Songs" row, and it means a different album
     * under a different filter — so a row-keyed cache aliases across filters. */
    artcache_queue(a, e->thm_clus, e->thm_size, e->art_clus, e->art_size);
    const uint16_t *chip = artcache_get(a);
    if (!chip) {
        /* DIAGNOSTIC: tint the placeholder by WHY there are no pixels, so a
         * blank chip is self-describing on a device with no serial cable.
         *   grey  = still coming (queued)
         *   amber = no sidecar cluster was ever resolved for this album
         *   red   = tried to load it and gave up
         * Remove once the first-album blank is understood. */
        switch (artcache_state(a)) {
        case ARTCACHE_ST_NOCLUS: chip = g_chip_noclus; break;
        case ARTCACHE_ST_NOART:  chip = g_chip_noart;  break;
        default:                 chip = g_chip_ph;     break;
        }
    }
    /* Show "Album" as the title and the artist as a sub-line (parsed from the
     * "Artist - Album" folder name). In an artist's own list the artist sub
     * is redundant, so drop it there. */
    char artist[NAME_MAX + 1], album[NAME_MAX + 1];
    split_artist_album(e->folder, artist, album);
    const char *sub = (!g_artist_filter[0] && artist[0]) ? artist : 0;
    list_row_tall(r, album, sub, 0, 1, idx == g_br_sel, 0, chip);
}

/* Album LIST (browser depth 0): the folder list with the design chrome, each
 * album row carrying a 22x22 cover chip (or a placeholder until it loads). */
static void albumlist_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    int total = albumlist_count();
    if (total > 0) {
        fmt_count(right, sel + 1, total);
    } else {
        right[0] = '\0';
    }
    /* Header title = the artist when drilled in from Artists, else "Albums". */
    ui_header(g_artist_filter[0] ? g_artist_filter : "Albums", right, 1);

    if (total == 0) {
        ui_text(14, LIST_Y0 + 20, "No albums", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, total, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= total) break;
        albumlist_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, total);
}

void browse_render(int sel)
{
    if (g_dir_depth > 0) {
        detail_render(sel);
    } else {
        albumlist_render(sel);
    }
}

/* Why the last browse_load produced what it did: 0, or the FAT32_* code of a
 * directory read that failed even after retries. An empty g_browse with a
 * nonzero code is an album that COULD NOT BE READ, not an album with no
 * tracks; the tracklist screen should say so (and offer Back), because the
 * two look identical otherwise and one of them is a disk fault. */
int g_browse_err;

/*
 * Which library song each g_browse row IS — a g_songs index, or -1 for a file
 * the library has no record of — and the row's ordering key. Both filled by
 * browse_bind() at the end of every browse_load, so the tracklist screen,
 * the album queue and the resume path all read the same answer.
 *
 * Rows bind to songs by FILE CLUSTER: the resolve pass already bound each
 * record to its directory entry (by file_hash), and the row was made from
 * that same entry, so the cluster is the one fact both sides hold exactly.
 * Matching by name was what this replaces — the row's ext-trimmed name
 * against the record's file[] field, which for a long filename is a
 * truncated string no row could equal, so those rows showed no duration,
 * no title and a made-up gutter number while the same file played fine.
 */
int16_t  g_browse_song[BROWSE_MAX];
static uint32_t g_browse_key[BROWSE_MAX];

static int browse_key_cmp_idx(uint16_t a, uint16_t b)
{
    uint32_t ka = g_browse_key[a], kb = g_browse_key[b];
    return (ka > kb) - (ka < kb);
}

static void browse_bind(uint32_t dir_clus)
{
    int n = g_browse_n;
    for (int i = 0; i < n; i++) {
        g_browse_song[i] = -1;
        g_browse_key[i]  = 0xFFFFFFFFu;       /* unbound rows sort last */
        g_browse[i].order_key = 0xFFFFFFFFu;  /* the row carries it into the
                                               * queue (Shuffle Albums)      */
    }
    /* One pass over the library: the album's songs are the ones whose folder
     * is this one, and each finds its row by cluster among at most BROWSE_MAX.
     * ~6000 integer compares plus a few hundred per album open — an event
     * that just read a directory off the disk. */
    for (int s = 0; s < g_songs_n; s++) {
        const lib_song_t *sg = &g_songs[s];
        if (sg->dir_clus != dir_clus || sg->file_clus == 0) continue;
        for (int i = 0; i < n; i++) {
            if (g_browse[i].is_dir || g_browse[i].clus != sg->file_clus) continue;
            g_browse_song[i] = (int16_t)s;
            g_browse_key[i]  = ((uint32_t)sg->disc << 16) | sg->track;
            /* Written onto the row as well as into the sort key: the album
             * queue is a copy of g_browse, and Shuffle Albums orders a group
             * by exactly this. The swap loop below carries it along with the
             * rest of the entry. */
            g_browse[i].order_key = g_browse_key[i];
            break;
        }
    }

    /*
     * Order the rows by the index's (disc, track). They arrive in raw FAT
     * directory order — whatever order the importer happened to copy the
     * files in — while the gutter prints the record's track number, so an
     * album whose numbers did not come from the filenames read 7, 2, 11 down
     * the screen. One authority: the index. Ties, and rows the index does not
     * know, keep directory order (the sort is stable; unbound keys are max).
     * The queue is built from g_browse in this order too, so the album PLAYS
     * in it as well as listing in it.
     *
     * Applied in place by following cycles, inverted first — the same steps
     * as library_finish, for the same reason (the sort yields a gather order
     * and the swap loop scatters).
     */
    static uint16_t order[BROWSE_MAX];
    for (int i = 0; i < n; i++) order[i] = (uint16_t)i;
    merge_sort_idx(order, n, g_sort_tmp, browse_key_cmp_idx);
    uint16_t *inv = g_sort_tmp;             /* free again once the sort is done */
    for (int k = 0; k < n; k++) inv[order[k]] = (uint16_t)k;
    for (int i = 0; i < n; i++) {
        while (inv[i] != (uint16_t)i) {
            int j = inv[i];
            browse_entry_t te = g_browse[i];
            g_browse[i] = g_browse[j];
            g_browse[j] = te;
            int16_t ts = g_browse_song[i];
            g_browse_song[i] = g_browse_song[j];
            g_browse_song[j] = ts;
            uint32_t tk = g_browse_key[i];
            g_browse_key[i] = g_browse_key[j];
            g_browse_key[j] = tk;
            uint16_t ti = inv[i];
            inv[i] = inv[j];
            inv[j] = ti;
        }
    }
}

/* Read an album's tracklist (the folder at `dir_clus`) into g_browse, bound to
 * the library and in the index's order (browse_bind). Only ever called at
 * depth 1 now — the album LIST is the index-driven g_albums. */
void browse_load(fat32_t *fs, uint32_t dir_clus)
{
    g_list_epoch++;
    g_cur_dir  = dir_clus;
    g_browse_n = 0;
    g_art_clus = 0;                      /* re-captured by browse_collect below */
    g_art_size = 0;
    /* One directory, opened by hand: give it the full set of retries rather
     * than whatever the boot load left in the budget — the budget exists to
     * bound a thousand-album boot, not a single user-initiated open. */
    g_lib_retry_budget = LIB_READDIR_RETRIES;
    g_browse_err = lib_readdir(fs, dir_clus, browse_collect, 0, &g_browse_n);
    if (g_browse_err != 0) {
        g_art_clus = g_art_size = 0;     /* nothing from a walk we don't trust */
        return;
    }
    /* The folder just read cleanly. If the load-time pass could not read it,
     * this is the re-attempt the flag promised: resolve it now, so its songs
     * become playable from Songs/Genres/shuffle and the About count drops.
     * Before the bind, which needs the clusters the resolve installs. */
    int ai = album_by_clus(dir_clus);
    if (ai >= 0 && g_albums[ai].unreadable) (void)album_resolve(fs, ai);
    browse_bind(dir_clus);
}

/* After entering an album folder: load its hero art, pull each track's
 * disc/duration/number from the index (matched by folder+filename), and build
 * the display view with "Disc N" section headers for a multi-disc album. */
void detail_load_meta(fat32_t *fs)
{
    detail_art_load(fs);
    g_album_track_n = 0;
    int maxd = 0;
    for (int i = 0; i < g_browse_n; i++) {
        g_track_dur[i] = 0;
        g_track_disc[i] = 0;
        g_track_num[i] = 0;
        g_track_title[i] = 0;
        if (g_browse[i].is_dir) continue;
        g_album_track_n++;
        /* The row's song, bound by cluster in browse_bind. No longer gated on
         * "loaded from the index": a scanned library binds the same way and
         * has durations and numbers of its own to show. */
        int s = g_browse_song[i];
        if (s >= 0) {
            g_track_dur[i]  = (uint16_t)g_songs[s].duration_s;
            g_track_disc[i] = (uint8_t)g_songs[s].disc;
            g_track_num[i]  = g_songs[s].track;
            if (g_songs[s].title[0]) g_track_title[i] = g_songs[s].title;
            if (g_songs[s].disc > maxd) maxd = g_songs[s].disc;
        }
    }
    g_detail_multidisc = (maxd > 1);
    /* Sum the runtime once, here — detail_render used to re-add every track's
     * duration on every paint just to print one meta line. */
    g_detail_total_s = 0;
    for (int i = 0; i < g_browse_n; i++) g_detail_total_s += g_track_dur[i];
    g_det_view_n = 0;
    int prev = -1;
    for (int i = 0; i < g_browse_n && g_det_view_n < DET_VIEW_MAX - 1; i++) {
        if (g_browse[i].is_dir) continue;
        int d = g_track_disc[i];
        if (g_detail_multidisc && d > 0 && d != prev) {
            g_det_view[g_det_view_n++] = (int16_t)(-(d + 1));   /* Disc header */
            prev = d;
        }
        g_det_view[g_det_view_n++] = (int16_t)i;
    }
}

/* Rows past the window the cover pump also loads, once every visible row is
 * resident: a screen ahead in the direction of travel and a screen behind.
 * Without them every newly revealed row waited for its read and then
 * re-presented its band when the cover landed: two paints per detent. */
#define CHIP_AHEAD_ROWS        LIST_ROWS2

/* Load album covers while on the album list. Idle → blast several per
 * pass (covers fill near-instantly). While a song plays we still spread
 * the I/O so it can't starve decode, but a folder.thm is only ~1KB and
 * the anti-skip diskbuf is 8MB (~73s), so a small BATCH every ~60ms
 * (~50 covers/s) is safe and stops the old one-line-at-a-time trickle.
 *
 * Every load is a SYNCHRONOUS disk read on this loop, so two things are
 * held back while the wheel is actually turning:
 *
 *   - A PARKED platter is not woken for a cover. player.c parks the
 *     drive between refill bursts (most of a track) and the idle timer
 *     parks it after 20 s, so the first cover read after either is a
 *     spin-up: the loop is frozen for the 1-3 s it takes (ATA_SPINUP_US
 *     allows 4), the wheel's motion piles into the latch, and the list
 *     jumps when it comes back. Covers wait for CHIP_SPINUP_QUIET_US of
 *     wheel silence, so that stall lands on a still screen — a wait, not
 *     a jerk — and a slow deliberate scroll never triggers it at all.
 *   - With the platter up, a scroll in progress gets ONE read per pass.
 *     Six reads is ~60 ms of seek+PIO stacked onto the pass that also
 *     has to paint the new window; one bounds the hitch and the rest of
 *     the window fills the moment the wheel settles.
 *
 * The pump is told the window (artcache_pump_for), so whichever reads
 * do happen go to the rows on screen, top first — never to rows that
 * already scrolled past. */
/* Called by run_ui once per pass while the album list is up. Returns the
 * visible rows (bit r: row r) whose chip appeared this pass; the caller
 * marks them in g_lp.rows and sets its dirty flag. *last_chip is the last
 * batch's timer stamp (run_ui's L->last_chip). */
uint32_t albumlist_pump_chips(fat32_t *fs, int locked, uint32_t *last_chip)
{
    uint32_t rows = 0;
    uint32_t nowc = mmio_read32(USEC_TIMER_ADDR);
    int idle_now  = !player_active();
    uint32_t since_wheel = nowc - wheel_last_us();
    int held;
    int budget = coverpump_budget(ata_is_parked(), locked, since_wheel, idle_now,
                                  (uint32_t)(nowc - *last_chip), &held);
    if (held) g_ui_art_held++;
    if (budget > 0) {
        /* A pumped slot is only worth a repaint if it is actually ON
         * SCREEN: pumping a whole library's covers used to set dirty for
         * every one of them, forcing ~21 consecutive full-frame repaints
         * for rows nobody was looking at. Snapshot the visible rows'
         * chips, pump, and repaint only the rows whose chip appeared —
         * through g_lp.rows, so a landed cover costs one row band, not a
         * cleared panel and a full-frame push (see list_repaint_partial). */
        const uint16_t *before[LIST_ROWS2];
        int want[LIST_ROWS2 + 2 * CHIP_AHEAD_ROWS];
        int nlist = albumlist_count();
        int vtop = ui_scroll_window(g_br_sel, nlist, LIST_ROWS2);
        /* PEEK, not get: this loop is only observing. artcache_get
         * claims a way and re-stamps the LRU, so using it here (12
         * times a pass, always in row order) is what pinned the top
         * row to the lowest stamp and starved it. The render's own
         * artcache_get calls are what should drive residency. */
        /* Map row -> global album index: the cache is keyed by album,
         * and a row is not an album when an artist filter puts the
         * synthetic "All Songs" row at 0. */
        for (int r = 0; r < LIST_ROWS2; r++) {
            want[r]   = albumlist_album_at(vtop + r);
            before[r] = (want[r] >= 0) ? artcache_peek(want[r]) : 0;
        }
        /* Then the look-ahead, after the window so it never delays a
         * visible chip: a screen in the direction the selection last
         * moved, then a screen the other way. Only from a drive that
         * is already turning: rows nobody is looking at are not worth
         * a spin-up (or, idle, the battery a spin-up costs). */
        int nwant = LIST_ROWS2;
        if (g_br_sel != g_chip_last_sel) {
            g_chip_dir = (g_br_sel > g_chip_last_sel) ? 1 : -1;
            g_chip_last_sel = g_br_sel;
        }
        if (!ata_is_parked()) {
            for (int pass = 0; pass < 2; pass++) {
                int dir = pass == 0 ? g_chip_dir : -g_chip_dir;
                for (int k = 0; k < CHIP_AHEAD_ROWS; k++) {
                    int row = dir > 0 ? vtop + LIST_ROWS2 + k
                                      : vtop - 1 - k;
                    if (row < 0 || row >= nlist) break;
                    want[nwant++] = albumlist_album_at(row);
                }
            }
        }
        uint32_t t_art = mmio_read32(USEC_TIMER_ADDR);
        int loaded = 0;
        while (loaded < budget && artcache_pump_for(fs, want, nwant)) {
            loaded++;
        }
        t_art = mmio_read32(USEC_TIMER_ADDR) - t_art;
        g_ui_art_reads += (uint32_t)loaded;
        if (loaded) drivepower_note_ui_read(&g_drivepower, mmio_read32(USEC_TIMER_ADDR));
        if (loaded && t_art > g_ui_art_max_us) g_ui_art_max_us = t_art;
        for (int r = 0; r < LIST_ROWS2; r++) {
            const uint16_t *now_px = (want[r] >= 0) ? artcache_peek(want[r]) : 0;
            if (now_px != before[r]) {
                rows |= 1u << r;
            }
        }
        *last_chip = nowc;
    }
    return rows;
}
