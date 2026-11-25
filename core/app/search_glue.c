/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/search_glue.c — Music > Search's binding to the library and to
 * navigation. Moved verbatim out of kernel/main.c.
 */

#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/volume.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../kernel/resume_ctx.h"
#include "../ui/text.h"
#include "../ui/chrome.h"
#include "../ui/search.h"
#include "../library/names.h"
#include "state.h"
#include "nav.h"
#include "library.h"
#include "playctx.h"
#include "statusbar.h"
#include "playlists.h"
#include "albums.h"
#include "main_internal.h"
#include "search_glue.h"

/* Music > Search's whole state, one instance. Up here rather than with the
 * rest of the screen because play_tap_start's policy switch reads it, and
 * that sits above the loaders the Search section is built on. */
search_t g_search;

/* ---------------------------------------------------------------------------
 * Music > Search (ui/search.c)
 *
 * The module owns the query, the ring, the match and the screen; main.c owns
 * the two things only it knows — WHAT to scan and how a hit reads as a row —
 * and what a chosen hit does, which is always "the thing the ordinary list
 * would have done", so a search result is never a second-class way in.
 * ------------------------------------------------------------------------- */
/* The name each source list is ORDERED by, which is also what its row shows.
 * Songs are addressed by SORTED position, so song hits arrive in title order
 * and the row lookup is one array step. */
static const char *search_name_of(int type, int i)
{
    switch (type) {
    case SEARCH_T_ARTIST:
        return (i >= 0 && i < g_artists_n)   ? g_artists[i].name   : 0;
    case SEARCH_T_ALBUM:
        return (i >= 0 && i < g_albums_n)    ? g_album_key[i]      : 0;
    case SEARCH_T_PLAYLIST:
        return (i >= 0 && i < g_playlists_n) ? g_playlists[i].name : 0;
    case SEARCH_T_SONG:
        return (i >= 0 && i < g_songs_n) ? g_songs[g_song_sorted[i]].title : 0;
    default:
        return 0;
    }
}

/* The key that list is SORTED by: for artists, past a leading "The ", so
 * typing "laroi" ranks The Kid LAROI as a prefix hit rather than burying it
 * under every title with the word in the middle. A pointer INTO the name, as
 * ui/search.h requires. */
static const char *search_artist_key_of(int type, int i)
{
    const char *n = search_name_of(type, i);
    if (!n) return 0;
    return (type == SEARCH_T_ARTIST) ? artist_key(n) : n;
}

static search_source_t g_search_src = {
    { 0, 0, 0, 0 }, search_name_of, search_artist_key_of
};

/* A hit's index only means anything against the list the scan ran over. The
 * library cannot change while the firmware runs, but the hits outlive the
 * screen, so every use of one is bounds-checked rather than trusted. */
static int search_hit_ok(const search_hit_t *h)
{
    int i = (int)h->idx;
    switch (h->type) {
    case SEARCH_T_ARTIST:   return i < g_artists_n;
    case SEARCH_T_ALBUM:    return i < g_albums_n;
    case SEARCH_T_PLAYLIST: return i < g_playlists_n;
    case SEARCH_T_SONG:     return i < g_songs_n;
    default:                return 0;
    }
}

static void sr_copy(char *dst, int cap, const char *src)
{
    int n = 0;
    while (src && src[n] && n < cap - 1) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

static int sr_cat(char *dst, int n, int cap, const char *src)
{
    while (src && *src && n < cap - 1) dst[n++] = *src++;
    dst[n] = '\0';
    return n;
}

/* Title, eyebrow and right-hand value for one hit. The eyebrow is the Now
 * Playing "TRACK n OF m" style — FONT_SMALL, muted — because the row has to
 * say WHAT it is before it says which: four kinds share one list here. */
static void search_row_fill(const search_hit_t *h, char *title, char *sub,
                            char *right, int *greyed)
{
    if (!search_hit_ok(h)) {
        return;                        /* left empty; the caller pre-cleared */
    }
    switch (h->type) {
    case SEARCH_T_ARTIST:
        sr_copy(title, SEARCH_ROW_MAX, g_artists[h->idx].name);
        sr_copy(sub,   SEARCH_ROW_MAX, "ARTIST");
        break;
    case SEARCH_T_ALBUM: {
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(g_albums[h->idx].folder, artist, album);
        sr_copy(title, SEARCH_ROW_MAX, g_album_key[h->idx]);
        int n = sr_cat(sub, 0, SEARCH_ROW_MAX, "ALBUM");
        if (artist[0]) {
            n = sr_cat(sub, n, SEARCH_ROW_MAX, " " UI_GLYPH_MIDDOT " ");
            (void)sr_cat(sub, n, SEARCH_ROW_MAX, artist);
        }
        break;
    }
    case SEARCH_T_PLAYLIST:
        sr_copy(title, SEARCH_ROW_MAX, g_playlists[h->idx].name);
        sr_copy(sub,   SEARCH_ROW_MAX, "PLAYLIST");
        break;
    default: {                                         /* SEARCH_T_SONG */
        const lib_song_t *sg = &g_songs[g_song_sorted[h->idx]];
        sr_copy(title, SEARCH_ROW_MAX, sg->title[0] ? sg->title : sg->file);
        int n = sr_cat(sub, 0, SEARCH_ROW_MAX, "SONG");
        if (sg->artist[0]) {
            n = sr_cat(sub, n, SEARCH_ROW_MAX, " " UI_GLYPH_MIDDOT " ");
            (void)sr_cat(sub, n, SEARCH_ROW_MAX, sg->artist);
        }
        if (sg->duration_s) fmt_time(right, sg->duration_s);
        /* A record the disk no longer backs cannot be played. Greyed, and
         * SELECT clicks without acting — the row is still evidence the song
         * was there, which is worth more than hiding it. */
        *greyed = (sg->file_clus == 0);
        break;
    }
    }
}

/*
 * Rerun the match, and say what it cost. The estimate in ui/search.h is
 * 25-55 ms per keystroke on this CPU and nothing has measured it on the
 * device; the UART line is what turns that into a number the bench can read,
 * and it costs one timer read either side of the scan.
 */
void search_rescan(void)
{
    g_search_src.count[SEARCH_T_ARTIST]   = g_artists_n;
    g_search_src.count[SEARCH_T_ALBUM]    = g_albums_n;
    g_search_src.count[SEARCH_T_PLAYLIST] = g_playlists_n;
    g_search_src.count[SEARCH_T_SONG]     = g_songs_n;
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    search_scan(&g_search, &g_search_src);
    uint32_t us = mmio_read32(USEC_TIMER_ADDR) - t0;
    uart_puts("core: search ");  uart_dec(g_search.qlen);
    uart_puts(" chars ");        uart_dec(g_search.total);
    uart_puts(" hits ");         uart_dec((int)(us / 1000u));
    uart_puts(" ms\n");
}

void search_render_cur(void)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    search_render(&g_search, search_row_fill);
}

/*
 * Load album `ai`'s tracklist into g_browse, its metadata and its art, and
 * point the album list at it. Unfiltered, so there is no synthetic All Songs
 * row and the list row for album `ai` IS `ai` — the album the user picked is
 * the one the list would land on if they ever backed out to it. The depth is
 * borrowed for the read the way resume_open_album does it: browse_collect
 * only lists files at depth 1.
 */
static void search_load_album(fat32_t *fs, int ai)
{
    g_artist_filter[0] = '\0';
    albumview_build(0);
    g_br_sel    = ai;
    g_br_accum  = 0;
    g_dir_depth = 1;
    split_artist_album(g_albums[ai].folder, g_album_artist, g_album_title);
    browse_load(fs, g_albums[ai].clus);
    detail_load_meta(fs);
    g_det_sel = g_det_accum = 0;
}

/*
 * Start a song hit in its ALBUM, at that song. 1 when the queue is running.
 *
 * The album and not the six-thousand-row Songs view: "play this one" means
 * this one in its record, it resumes through the existing RESUME_KIND_ALBUM
 * path, and it skips the LOADING SONGS bar. That reuses g_browse, which is
 * only safe while no tracklist is on the stack underneath — Search is
 * reachable from Music alone, so it is; the UART line is what will say so if
 * that ever stops being true.
 */
static int search_play_song(fat32_t *fs, const search_hit_t *h)
{
    const lib_song_t *sg = &g_songs[g_song_sorted[h->idx]];
    if (!sg->file_clus) {
        return 0;                       /* greyed: nothing on the disk to play */
    }
    for (int i = 0; i < g_scr_n; i++) {
        if (g_scr[i] == SCR_BROWSER) {
            uart_puts("core: search play under a browser, tracklist lost\n");
            break;
        }
    }
    int saved_depth = g_dir_depth;
    g_dir_depth = 1;                /* browse_collect lists FILES at depth 1 */
    browse_load(fs, sg->dir_clus);
    g_dir_depth = saved_depth;
    /* The row is the file the record bound to: same cluster, never the name
     * (see resume_open_album, which binds the same way). */
    int row = -1;
    for (int i = 0; i < g_browse_n; i++) {
        if (!g_browse[i].is_dir && g_browse[i].clus == sg->file_clus) {
            row = i;
            break;
        }
    }
    if (row < 0) {
        return 0;                   /* the folder no longer holds the file */
    }
    g_queue_kind = RESUME_KIND_ALBUM;
    g_queue_seed = 0;
    player_play_queue(g_browse, g_browse_n, row, g_art_clus, g_art_size);
    return 1;
}

/*
 * Act on the selected hit. Every case is the block the ordinary list runs, so
 * a result behaves exactly like the row it stands for. Returns 1 when Now
 * Playing was pushed (the caller arms its first-paint flag).
 */
int search_open_hit(fat32_t *fs)
{
    if (g_search.sel < 0 || g_search.sel >= g_search.nhit) return 0;
    const search_hit_t *h = &g_search.hit[g_search.sel];
    if (!search_hit_ok(h)) return 0;

    switch (h->type) {
    case SEARCH_T_ARTIST: {
        int k = 0;
        for (; g_artists[h->idx].name[k] && k < NAME_MAX; k++) {
            g_artist_filter[k] = g_artists[h->idx].name[k];
        }
        g_artist_filter[k] = '\0';
        g_dir_depth = 0;
        albumview_build(g_artist_filter);
        albumlist_queue_chips();
        g_br_sel = g_br_accum = 0;
        g_br_from_search = 0;           /* the album list IS the screen here */
        scr_push(SCR_BROWSER);
        return 0;
    }
    case SEARCH_T_ALBUM:
        search_load_album(fs, h->idx);
        g_br_from_search = 1;
        scr_push(SCR_BROWSER);
        return 0;
    case SEARCH_T_PLAYLIST:
        playlist_open(fs, h->idx);
        scr_push(SCR_PLAYLIST);
        return 0;
    default:                                           /* SEARCH_T_SONG */
        if (!search_play_song(fs, h)) {
            return 0;
        }
        hal_volume_set(g_volume);   /* re-apply over the codec re-init */
        scr_push(SCR_NOWPLAYING);
        return 1;
    }
}

/*
 * PLAY on a result row — the gesture, not the button. Every case is what PLAY
 * does on the list that row came from: an artist plays their whole
 * discography, an album plays from its first track, a playlist from its
 * first, a song plays in its album. Returns PLAY_TAP_STARTED with the queue
 * running and the push left to play_tap_start's shared tail.
 */
play_tap_t search_play_hit(fat32_t *fs)
{
    if (g_search.sel < 0 || g_search.sel >= g_search.nhit) return PLAY_TAP_PASS;
    const search_hit_t *h = &g_search.hit[g_search.sel];
    if (!search_hit_ok(h)) return PLAY_TAP_PASS;

    switch (h->type) {
    case SEARCH_T_ARTIST: {
        int k = 0;
        for (; g_artists[h->idx].name[k] && k < NAME_MAX; k++) {
            g_artist_filter[k] = g_artists[h->idx].name[k];
        }
        g_artist_filter[k] = '\0';
        songview_build(-1, g_artist_filter);
        if (library_play_song(fs, 0) < 0) return PLAY_TAP_CONSUMED;
        break;
    }
    case SEARCH_T_ALBUM:
        search_load_album(fs, h->idx);
        if (g_browse_err != 0 || g_browse_n == 0) {
            /* Unreadable or empty: put the tracklist screen up with the reason,
             * exactly as SELECT would, and do not ALSO toggle a transport that
             * never started. */
            g_br_from_search = 1;
            scr_push(SCR_BROWSER);
            return PLAY_TAP_CONSUMED;
        }
        g_queue_kind = RESUME_KIND_ALBUM;
        g_queue_seed = 0;
        player_play_queue(g_browse, g_browse_n, 0, g_art_clus, g_art_size);
        g_dir_depth = 0;            /* no BROWSER under this: Search is */
        break;
    case SEARCH_T_PLAYLIST:
        playlist_open(fs, h->idx);
        if (playlist_play(0) < 0) {
            scr_push(SCR_PLAYLIST); /* the screen that says why, as SELECT */
            return PLAY_TAP_CONSUMED;
        }
        break;
    default:                                           /* SEARCH_T_SONG */
        /* Greyed, or the folder no longer holds the file: the press was aimed
         * at this row and declined, and the player was never touched. */
        if (!search_play_song(fs, h)) return PLAY_TAP_CONSUMED;
        break;
    }
    return PLAY_TAP_STARTED;
}
