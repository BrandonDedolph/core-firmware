/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/lists.h — the plain list screens: the main menu, the Music menu,
 * Artists, Songs, Genres and the Queue. Each is a cursor, a row painter
 * (the list_view_t row signature the partial repaint uses) and a full
 * renderer. Moved verbatim out of kernel/main.c.
 *
 * The cursors and the menu tables are plain extern data for now (phase 2 of
 * the split): run_ui moves them with the wheel and acts on them, and
 * list_view_current points the partial repaint at them.
 */

#ifndef CORE_APP_LISTS_H
#define CORE_APP_LISTS_H

#include <stdint.h>

typedef struct { const char *label; uint8_t active; } menu_item_t;

/* Main menu rows. */
enum { MM_MUSIC, MM_PLAYLISTS, MM_PODCASTS, MM_AUDIOBOOKS, MM_SETTINGS,
       MM_NOWPLAYING, MM_COUNT };

/* Music sub-menu rows. */
enum { MU_PLAYLISTS, MU_ARTISTS, MU_ALBUMS, MU_SONGS, MU_SHUFFLE, MU_GENRES,
       MU_SEARCH, MU_COMPOSERS, MU_AUDIOBOOKS, MU_COUNT };

extern int  g_artist_sel, g_artist_accum;
extern int        g_song_sel, g_song_accum;
extern int        g_genre_sel, g_genre_accum;
extern int g_queue_sel, g_queue_accum;

extern menu_item_t g_main_menu[MM_COUNT];
extern int g_main_sel;
extern const menu_item_t g_music_menu[MU_COUNT];
extern int g_music_sel;
extern int g_menu_accum;                   /* shared by both menus */
extern const menu_item_t *g_menu_items;
extern int                g_menu_sel;

void artists_row_draw(int r, int idx);
void artists_render(int sel);
void songs_row_draw(int r, int idx);
void songs_render(int sel);
void genres_row_draw(int r, int idx);
void genres_render(int sel);
void queue_row_draw(int r, int idx);
void queue_render(int sel);
void menu_row_draw(int r, int idx);
int main_menu_count(void);
void main_menu_render(void);
void music_menu_render(void);

#endif /* CORE_APP_LISTS_H */
