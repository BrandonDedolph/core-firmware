/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/nav.h — the screen stack: which screen is up and the path back
 * to the main menu, plus g_list_epoch, the counter every list rebuild and
 * every stack move bumps. Moved verbatim out of kernel/main.c. g_scr and
 * g_scr_n are plain extern data: run_ui resets and truncates the stack
 * directly and search_play_song scans it.
 */

#ifndef CORE_APP_NAV_H
#define CORE_APP_NAV_H

#include <stdint.h>

extern uint32_t g_list_epoch;

typedef enum { SCR_MENU, SCR_MUSIC, SCR_ARTISTS, SCR_SONGS, SCR_GENRES,
               SCR_PLAYLISTS, SCR_PLAYLIST, SCR_OTG, SCR_SEARCH,
               SCR_BROWSER, SCR_NOWPLAYING, SCR_QUEUE, SCR_SETTINGS,
               SCR_BATTERY, SCR_CHARGING } screen_t;
/* The deepest legal path is 8: MENU, MUSIC, ARTISTS, BROWSER, SONGS,
 * NOWPLAYING, QUEUE, plus ONE modal (Search is no deeper: it sits where
 * ARTISTS does, and the BROWSER an album hit opens sits where BROWSER does;
 * On-The-Go is no deeper either — MENU, PLAYLISTS, OTG, NOWPLAYING, QUEUE
 * plus a modal is 6) (scr_push_modal replaces a modal with a
 * modal, so BATTERY and CHARGING never stack). The headroom is deliberate:
 * this used to be exactly 8 without SONGS counted, and a DISKSAFE edge at
 * the bottom of that path retried the push every pass with dirty set — a
 * full-repaint loop until a button cleared it. A new screen still deserves
 * a look at the arithmetic; the UART line is what says it was wrong. RIGHT
 * on a list pushes NOWPLAYING where a SELECT would have, so the jump adds no
 * depth beyond the path above. */
#define SCR_STACK_MAX 12
extern screen_t g_scr[SCR_STACK_MAX];
extern int      g_scr_n;

void      scr_push(screen_t s);
void      scr_pop(void);
screen_t  scr_cur(void);
int       scr_is_modal(screen_t s);
void      scr_push_modal(screen_t s);
int scr_pop_to_root(void);

#endif /* CORE_APP_NAV_H */
