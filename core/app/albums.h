/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/albums.h — the album list (browser depth 0, index-driven, one
 * cover chip per row) and the album detail (depth 1: hero art and the
 * folder's tracklist), and the folder browse data behind them: g_browse, the
 * live read of one album folder, bound to the library's songs and put in the
 * index's order. Moved verbatim out of kernel/main.c.
 *
 * The browse state is plain extern data for now (phase 2 of the split):
 * run_ui, the queue builders, search and resume read and reset it directly.
 * g_dir_depth doubles as "which of the two browser screens is up".
 */

#ifndef CORE_APP_ALBUMS_H
#define CORE_APP_ALBUMS_H

#include <stdint.h>

#include "hw/clickwheel.h"
#include "../fs/fat32.h"
#include "../player/player.h"   /* browse_entry_t, BROWSE_MAX, NAME_MAX */
#include "ui_loop.h"

/* Album detail tracklist geometry. */
#define DET_LIST_Y0  108                  /* tracklist first row top              */
#define DET_ROWS     5                     /* visible rows: (240-108)/24 = 5 fit   */
#define DET_VIEW_MAX (BROWSE_MAX + 8)
/* The tracklist's animated now-playing bars: bounding box. */
#define NP_BARS_W  9                          /* bounding box width  (x..x+8)      */
#define NP_BARS_H  9                          /* bounding box height (y..y+8)      */

extern browse_entry_t g_browse[BROWSE_MAX];
extern int            g_browse_n;
extern uint32_t g_cur_dir;
extern int      g_dir_depth;
extern int g_br_sel, g_br_accum;      /* album list (depth 0)  */
extern int g_chip_last_sel, g_chip_dir;  /* cover look-ahead's direction */
extern int g_br_from_search;
extern int g_det_sel, g_det_accum;    /* tracklist   (depth 1) */
extern uint32_t g_art_clus, g_art_size;
extern char g_artist_filter[NAME_MAX + 1];
extern int g_browse_err;
extern int16_t  g_browse_song[BROWSE_MAX];
extern char     g_album_title[NAME_MAX + 1];
extern char     g_album_artist[NAME_MAX + 1];
extern int16_t  g_det_view[DET_VIEW_MAX];
extern int      g_det_view_n;

void browse_load(fat32_t *fs, uint32_t dir_clus);
void detail_load_meta(fat32_t *fs);
uint32_t bars_clock(void);
void nowplaying_bars(int x, int y, uint16_t c, uint32_t t_us);
void detail_bars_tick(ui_loop_t *L);
void detail_row_draw(int r, int vi);
int detail_sel_view(int sel);
void chip_placeholder_init(void);
void albumlist_queue_chips(void);
uint32_t albumlist_pump_chips(fat32_t *fs, int locked, uint32_t *last_chip);
int albumlist_count(void);
int albumlist_album_at(int row);
void albumlist_row_draw(int r, int idx);
void browse_render(int sel);

/* SCR_BROWSER's input, at either depth: run_ui calls it with the drained
 * event, its loop state, the mounted volume and the event's tick while the
 * browser is on top. */
void browse_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                  uint32_t ev_us);

#endif /* CORE_APP_ALBUMS_H */
