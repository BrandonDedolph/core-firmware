/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/playlists.h — Music -> Playlists: the playlist list and one open
 * playlist's tracklist, their state, and playing one as a queue. Moved
 * verbatim out of kernel/main.c. The state is plain extern data: run_ui's
 * input switch moves the cursors, and On-The-Go, resume, search, the list
 * view table and the row actions read the open playlist's rows directly.
 */

#ifndef CORE_APP_PLAYLISTS_H
#define CORE_APP_PLAYLISTS_H

#include <stdint.h>

#include "../fs/fat32.h"
#include "../library/playlist.h"

extern playlist_t         g_playlists[PLAYLIST_MAX];
extern int                g_playlists_n;
extern int                g_playlists_err;
extern int                g_pl_sel, g_pl_accum;
extern int                g_playlists_scanned;

extern playlist_track_t   g_pl_tracks[PLAYLIST_TRACKS_MAX];
extern int16_t            g_pl_song[PLAYLIST_TRACKS_MAX];
extern int                g_pl_tracks_n;
extern int                g_pl_open;
extern int                g_plt_sel, g_plt_accum;
extern int                g_pl_slot;

uint32_t playlists_root_clus(fat32_t *fs);
void playlists_load(fat32_t *fs);
void playlists_ensure(fat32_t *fs);
void playlist_open(fat32_t *fs, int pi);
int  playlist_play(int start);

int  playlists_row_count(void);
void playlists_row_draw(int r, int idx);
void playlists_render(int sel);
int  playlist_row_count(void);
void playlist_row_draw(int r, int idx);
void playlist_render(int sel);

#endif /* CORE_APP_PLAYLISTS_H */
