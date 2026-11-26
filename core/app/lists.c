/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/lists.c — see lists.h.
 */

#include <stdint.h>
#include "hal.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../kernel/datetime.h"
#include "../ui/text.h"
#include "../ui/chrome.h"
#include "../ui/settings.h"
#include "../ui/wheel.h"
#include "../library/names.h"
#include "state.h"
#include "layout.h"
#include "library.h"
#include "statusbar.h"
#include "albums.h"
#include "clockui.h"
#include "nav.h"
#include "playlists.h"
#include "playctx.h"
#include "actions.h"
#include "search_glue.h"
#include "settings_glue.h"
#include "lists.h"

/* Slim right-edge scrollbar (menus.jsx Scrollbar); no-op when everything fits.
 * `y0` is the list origin (differs between the full list and the detail view). */
int  g_artist_sel, g_artist_accum;

void artists_row_draw(int r, int idx)
{
    list_row(r, g_artists[idx].name, 0, 0, 1, idx == g_artist_sel, 0, 0);
}

void artists_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_artists_n > 0) fmt_count(right, sel + 1, g_artists_n);
    else                 right[0] = '\0';
    ui_header("Artists", right, 1);

    if (g_artists_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No artists", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_artists_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_artists_n) break;
        artists_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, g_artists_n);
}

int        g_song_sel, g_song_accum;
int        g_genre_sel;
static int g_genre_accum;

/*
 * The album a song belongs to, resolved through its folder cluster and split
 * out of the "Artist - Album" folder name. Writes "" when the folder isn't in
 * the album table (a track whose folder never became an album entry).
 *
 * Through album_by_clus: the cluster->album hash already exists (the loader
 * builds it for the queue builders), so there is no second table to keep in
 * sync. This used to be its own linear scan over every album, run per visible
 * row per repaint — up to 1024 compares a row, six rows a frame, on the one
 * list the wheel moves fastest.
 */
static void song_album_title(const lib_song_t *sg, char *out)
{
    int ai = album_by_clus(sg->dir_clus);
    if (ai >= 0) {
        char artist[NAME_MAX + 1];
        split_artist_album(g_albums[ai].folder, artist, out);
        return;
    }
    out[0] = '\0';
}

void songs_row_draw(int r, int idx)
{
    lib_song_t *sg = &g_songs[g_songview[idx]];
    char dur[FMT_TIME_MAX];
    if (sg->duration_s) fmt_time(dur, sg->duration_s); else dur[0] = '\0';

    /*
     * Sub-line: normally the artist, because the unfiltered Songs list mixes
     * every artist together and that is what tells rows apart.
     *
     * In an artist's "All Songs" the header ALREADY names the artist, so
     * repeating it on every row spends the only sub-line on the one fact the
     * user cannot need. Show the ALBUM there instead — in that view it is
     * exactly what distinguishes one row from the next.
     */
    char alb[NAME_MAX + 1];
    const char *sub = sg->artist[0] ? sg->artist : 0;
    if (g_songview_artist[0]) {
        song_album_title(sg, alb);
        sub = alb[0] ? alb : 0;
    }
    list_row_titled(r, sg->title, sub, dur[0] ? dur : 0, idx == g_song_sel, 0);
}

void songs_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_songview_n > 0) fmt_count(right, sel + 1, g_songview_n);
    else                  right[0] = '\0';
    ui_header(g_songview_artist[0] ? g_songview_artist : "Songs",
                  right, 1);
    if (g_songview_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No songs", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_songview_n, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= g_songview_n) break;
        songs_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, g_songview_n);
}

void genres_row_draw(int r, int idx)
{
    char cnt[8];
    u32_to_dec(cnt, (unsigned)g_genre_count[idx]);
    list_row(r, g_genres[idx], 0, cnt, 0, idx == g_genre_sel, 0, 0);
}

void genres_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_genres_n > 0) fmt_count(right, sel + 1, g_genres_n);
    else                right[0] = '\0';
    ui_header("Genres", right, 1);
    if (g_genres_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No genres", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_genres_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_genres_n) break;
        genres_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, g_genres_n);
}

/* ---------------------------------------------------------------------------
 * Queue view: the tracks queued in the current playback (the folder a track was
 * launched from). The playing track carries the animated now-playing bars;
 * SELECT jumps to a track. Reached via SELECT on the Now Playing screen.
 * ------------------------------------------------------------------------- */
int g_queue_sel, g_queue_accum;

void queue_row_draw(int r, int idx)
{
    int is_sel = (idx == g_queue_sel);
    list_row(r, track_display(player_queue_name(idx)), 0, 0, 0, is_sel,
             player_queue_is_dir(idx), 0);
    if (idx == player_queue_current()) {      /* the currently-playing track */
        int ry = LIST_Y0 + r * ROW_H;
        nowplaying_bars(LCD_WIDTH - 22, ry + 7,
                        is_sel ? LINEN_SEL_FG : LINEN_INK,
                        bars_clock());
    }
}

void queue_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    int n   = player_queue_len();
    int cur = player_queue_current();
    char right[12];
    if (n > 0) fmt_count(right, cur + 1, n);
    else       right[0] = '\0';
    ui_header("Now Playing", right, 1);

    if (n == 0) {
        ui_text(14, LIST_Y0 + 20, "Queue empty", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= n) break;
        queue_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, n);
}

/* ---------------------------------------------------------------------------
 * Menus (main + Music sub-menu)
 *
 * Both are the same widget over a small {label, active} item list. Inactive
 * items render greyed and SELECT does nothing (features not yet built). The
 * renderer reuses the list row geometry (LIST_Y0/ROW_H) and, since Search
 * took Music past LIST_ROWS, the same scroll window and scrollbar every other
 * list has. A menu that silently drops its ninth row is the worst way to find
 * out it grew.
 * ------------------------------------------------------------------------- */
/* Main menu. ACTIVE: Music, Now Playing (Now Playing greyed until something is
 * playing — its `active` flag is refreshed from the player before each paint). */
menu_item_t g_main_menu[MM_COUNT] = {
    { "Music",       1 },
    { "Playlists",   1 },
    { "Podcasts",    0 },
    { "Audiobooks",  0 },
    { "Settings",    1 },
    { "Now Playing", 1 },
};
int g_main_sel;

/* Music sub-menu. ACTIVE: Playlists, Artists, Albums, Songs, Shuffle Songs,
 * Genres; Composers and Audiobooks are greyed (no backing implementation). */
const menu_item_t g_music_menu[MU_COUNT] = {
    { "Playlists",     1 },
    { "Artists",       1 },
    { "Albums",        1 },
    { "Songs",         1 },
    { "Shuffle Songs", 1 },
    { "Genres",        1 },
    { "Search",        1 },
    { "Composers",     0 },
    { "Audiobooks",    0 },
};
int g_music_sel;

/* Shared wheel accumulator for the menu screens. */
int g_menu_accum;

/* Generic menu renderer over a {label, active} list. `back` draws the header
 * back chevron (off for the root menu). */
/* The item list + selection the menu rows are drawn from — set by
 * menu_render_list (and by list_view_current, for the partial repaint path)
 * so menu_row_draw has the same row-drawing signature as every other list. */
const menu_item_t *g_menu_items;
int                g_menu_sel;

void menu_row_draw(int r, int idx)
{
    list_row(r, g_menu_items[idx].label, 0, 0, 1 /*chevron*/, idx == g_menu_sel,
             !g_menu_items[idx].active /*greyed*/, 0);
}

static void menu_render_list(const char *title, const char *right,
                             const menu_item_t *items,
                             int n, int sel, int back)
{
    g_menu_items = items;
    g_menu_sel   = sel;
    console_clear(LINEN_SURFACE);
    status_strip_render();
    ui_header(title, right ? right : "", back);
    int top = ui_scroll_window(sel, n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= n) break;
        menu_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, n);
}

/* Visible main-menu row count: "Now Playing" (the last item) only appears while
 * a track is loaded, so it's simply dropped from the count when idle. */
int main_menu_count(void)
{
    return player_active() ? MM_COUNT : MM_COUNT - 1;
}

void main_menu_render(void)
{
    int n = main_menu_count();
    if (g_main_sel >= n) g_main_sel = n - 1;   /* cursor was on a now-gone row */
    /* The clock lives in the main menu's header slot, which is empty today and
     * which ui_header measures BEFORE the title — so "Core" (30 px) and a
     * clock cannot collide however wide the clock gets. This is the one place
     * the time is shown while a track is playing: the strip keeps the track
     * name (strip_left_text). */
    char clock[DATETIME_TIME_MAX];
    clock_title_text(clock, (int)sizeof clock);
    menu_render_list("Core", clock, g_main_menu, n, g_main_sel, 0);
}

void music_menu_render(void)
{
    menu_render_list("Music", NULL, g_music_menu, MU_COUNT, g_music_sel, 1);
}

/* The input half of each screen: what run_ui's per-screen switch does with
 * one drained wheel event while that screen is on top. Each body is the
 * switch case it replaced, statement for statement, with run_ui's loop
 * state reached through L (app/ui_loop.h). */

void main_menu_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                     uint32_t ev_us)
{
    (void)ev_us;
    if (ev->wheel_delta) {
        g_main_sel = wheel_move(g_main_sel, main_menu_count(),
                                ev->wheel_delta, &g_menu_accum);
        L->dirty = 1;
    }
    if (ev->buttons & WHEEL_BTN_SELECT) {
        if (g_main_sel == MM_MUSIC) {
            g_music_sel  = MU_ALBUMS;
            g_menu_accum = 0;
            scr_push(SCR_MUSIC);
        } else if (g_main_sel == MM_NOWPLAYING && player_active()) {
            scr_push(SCR_NOWPLAYING);
            L->np_first = 1;
        } else if (g_main_sel == MM_SETTINGS) {
            g_set_screen = SETTINGS_ROOT;
            g_set_sel = g_set_root_sel = g_set_accum = 0;
            g_set_editing = 0;
            scr_push(SCR_SETTINGS);
        } else if (g_main_sel == MM_PLAYLISTS) {
            playlists_ensure(fs);          /* same list as Music -> */
            scr_push(SCR_PLAYLISTS);       /* Playlists; MENU pops here */
        }
        /* other items are greyed: SELECT does nothing yet */
        L->dirty = 1;
    }
}

void music_menu_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                      uint32_t ev_us)
{
    (void)ev_us;
    if (ev->wheel_delta) {
        g_music_sel = wheel_move(g_music_sel, MU_COUNT,
                                 ev->wheel_delta, &g_menu_accum);
        L->dirty = 1;
    }
    if (ev->buttons & WHEEL_BTN_SELECT) {
        if (g_music_sel == MU_ALBUMS) {
            library_ensure(fs);            /* index -> g_albums      */
            g_dir_depth = 0;
            g_artist_filter[0] = '\0';     /* all albums             */
            albumview_build(0);
            albumlist_queue_chips();       /* start loading covers   */
            g_br_sel = g_br_accum = 0;
            g_br_from_search = 0;
            scr_push(SCR_BROWSER);
        } else if (g_music_sel == MU_ARTISTS) {
            library_ensure(fs);            /* index -> g_albums      */
            g_artist_filter[0] = '\0';
            /* No rebuild: library_ensure built the list when the
             * index loaded, and nothing changes g_albums after. */
            g_artist_sel = g_artist_accum = 0;
            scr_push(SCR_ARTISTS);
        } else if (g_music_sel == MU_SONGS) {
            library_ensure(fs);
            songview_build(-1, 0);         /* all songs, all artists */
            scr_push(SCR_SONGS);
        } else if (g_music_sel == MU_SHUFFLE) {
            shuffle_songs_play(fs);        /* the library, shuffled */
            hal_volume_set(g_volume);      /* re-apply over codec re-init */
            if (player_active()) {
                scr_push(SCR_NOWPLAYING);
                L->np_first = 1;
            }
        } else if (g_music_sel == MU_GENRES) {
            library_ensure(fs);
            g_genre_sel = g_genre_accum = 0;
            scr_push(SCR_GENRES);
        } else if (g_music_sel == MU_PLAYLISTS) {
            playlists_ensure(fs);          /* Music/Playlists/NAME.m3u8 */
            scr_push(SCR_PLAYLISTS);
        } else if (g_music_sel == MU_SEARCH) {
            library_ensure(fs);
            /* Artists are built by library_ensure. The playlist
             * folder read is once a session — the disk cannot
             * change under a running firmware — so only the
             * first entry here or into Playlists reads it. */
            if (!g_playlists_scanned) playlists_load(fs);
            /* The query survives leaving the screen, so re-scan
             * on the way back in: the hits then always name rows
             * of the lists as they are now. */
            if (g_search.qlen > 0) search_rescan();
            scr_push(SCR_SEARCH);
        }
        /* other items are greyed: SELECT does nothing yet */
        L->dirty = 1;
    }
    if (ev->buttons & WHEEL_BTN_MENU) {
        scr_pop();                          /* back to main menu */
        g_menu_accum = 0;
        L->dirty = 1;
    }
}

void artists_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                   uint32_t ev_us)
{
    (void)fs;
    (void)ev_us;
    if (ev->wheel_delta && g_artists_n > 0) {
        g_artist_sel = wheel_move(g_artist_sel, g_artists_n,
                                  ev->wheel_delta, &g_artist_accum);
        L->dirty = 1;
    }
    if ((ev->buttons & WHEEL_BTN_SELECT) && g_artists_n > 0) {
        /* Filter the album list to the chosen artist's folders. */
        int k = 0;
        for (; g_artists[g_artist_sel].name[k] && k < NAME_MAX; k++) {
            g_artist_filter[k] = g_artists[g_artist_sel].name[k];
        }
        g_artist_filter[k] = '\0';
        g_dir_depth = 0;
        albumview_build(g_artist_filter);
        albumlist_queue_chips();
        g_br_sel = g_br_accum = 0;
        g_br_from_search = 0;
        scr_push(SCR_BROWSER);
        L->dirty = 1;
    }
    if (ev->buttons & WHEEL_BTN_MENU) {
        scr_pop();                          /* back to Music menu */
        L->dirty = 1;
    }
}

void songs_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                 uint32_t ev_us)
{
    (void)fs;
    if (ev->wheel_delta && g_songview_n > 0) {
        g_song_sel = wheel_move(g_song_sel, g_songview_n,
                                ev->wheel_delta, &g_song_accum);
        L->dirty = 1;
    }
    if ((ev->buttons & WHEEL_BTN_SELECT) && g_songview_n > 0) {
        /* Tap plays, hold adds to On-The-Go: the press length
         * decides, in the block beside Now Playing's. */
        rowsel_arm(g_song_sel, &L->row_key, ev_us);
    }
    if (ev->buttons & WHEEL_BTN_MENU) {
        scr_pop();                          /* back (Music or Genres) */
        L->dirty = 1;
    }
}

void genres_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                  uint32_t ev_us)
{
    (void)fs;
    (void)ev_us;
    if (ev->wheel_delta && g_genres_n > 0) {
        g_genre_sel = wheel_move(g_genre_sel, g_genres_n,
                                 ev->wheel_delta, &g_genre_accum);
        L->dirty = 1;
    }
    if ((ev->buttons & WHEEL_BTN_SELECT) && g_genres_n > 0) {
        songview_build(g_genre_sel, 0);     /* this genre's songs */
        scr_push(SCR_SONGS);
        L->dirty = 1;
    }
    if (ev->buttons & WHEEL_BTN_MENU) {
        scr_pop();                          /* back to Music menu */
        L->dirty = 1;
    }
}

void queue_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                 uint32_t ev_us)
{
    (void)fs;
    (void)ev_us;
    if (ev->wheel_delta && player_queue_len() > 0) {
        g_queue_sel = wheel_move(g_queue_sel, player_queue_len(),
                                 ev->wheel_delta, &g_queue_accum);
        L->dirty = 1;
    }
    if ((ev->buttons & WHEEL_BTN_SELECT) && player_queue_len() > 0) {
        if (!player_queue_is_dir(g_queue_sel)) {
            player_jump(g_queue_sel);          /* play the chosen track */
            hal_volume_set(g_volume);          /* re-apply over re-init */
            scr_pop();                          /* back to Now Playing  */
        }
        L->dirty = 1;
    }
    if (ev->buttons & WHEEL_BTN_MENU) {
        scr_pop();
        L->dirty = 1;
    }
}
