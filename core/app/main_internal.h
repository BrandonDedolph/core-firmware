/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/main_internal.h — TRANSITIONAL. Symbols still defined in
 * kernel/main.c that an already-extracted core/app/ module needs.
 *
 * While main.c is split into modules one PR at a time (phase 2), a module
 * that moves out before one of its imports does reaches that import here:
 * the symbol loses `static` in main.c and gains a declaration below, tagged
 * with a trailing `until <module> moves` comment naming the module that
 * will own it.
 *
 * When that module moves, its declarations leave this file for the
 * module's own header. Nothing else belongs here. The rule: this file is
 * EMPTY and DELETED by the end of phase 2.
 */

#ifndef CORE_APP_MAIN_INTERNAL_H
#define CORE_APP_MAIN_INTERNAL_H

#include <stdint.h>

#include "util.h"

int enter_standby(void);                   /* until power moves */
extern uint32_t g_present_cost_us;         /* until paint moves */

extern ui_window_t g_pl_confirm;            /* until actions moves */

/*
 * PLAY_TAP_CONSUMED is the tap that was acted on and started nothing: a
 * playlist whose file cannot be read or whose tracks have all gone missing
 * (its tracklist screen is now up carrying the reason), an album folder that
 * could not be read (the same), or a view whose every song has vanished off
 * the disk since the index was built. That last one is the reason this is not
 * simply PASS: library_play_song() and shuffle_songs_build() clear the old
 * queue BEFORE they discover there is nothing to add, so the music has
 * already stopped and the status strip's track name has to repaint away. In
 * all of them the screen changed, and PLAY must not ALSO toggle a transport
 * that is no longer there.
 */
typedef enum {
    PLAY_TAP_PASS = 0,   /* not a title: PLAY still means pause/resume        */
    PLAY_TAP_STARTED,    /* a queue started and Now Playing is up             */
    PLAY_TAP_CONSUMED,   /* acted on, started nothing: repaint, do not toggle */
} play_tap_t;                               /* until actions moves */

#endif /* CORE_APP_MAIN_INTERNAL_H */
