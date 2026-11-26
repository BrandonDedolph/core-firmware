/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/actions.h — what PLAY and SELECT do on a list row: PLAY's tap
 * (play_tap_start), and SELECT's tap and hold, armed on the down-edge with
 * the row recorded (rowsel_*) and decided by run_ui's per-pass block
 * (row_select_tap / row_select_hold). Moved verbatim out of kernel/main.c.
 * g_rowsel is plain extern data: that per-pass block reads it to drop a
 * press whose screen, list or sub-state changed under the finger.
 */

#ifndef CORE_APP_ACTIONS_H
#define CORE_APP_ACTIONS_H

#include <stdint.h>

#include "../fs/fat32.h"
#include "../ui/keyhold.h"

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
} play_tap_t;

/* The press recorded at SELECT's down-edge; see the rowsel_* block in
 * actions.c. */
typedef struct {
    int      pending;
    int      scr;
    int      sub;        /* the screen's own sub-state (rowsel_sub)         */
    int      sel;        /* the row under the thumb at the down-edge        */
    uint32_t epoch;      /* g_list_epoch: the list must not have been rebuilt */
} rowsel_t;

extern rowsel_t g_rowsel;

/*
 * The SELECT bodies that used to sit in the event switch, keyed on the
 * RECORDED screen and row. The return is a little bitmask rather than a bool
 * because the caller has two things to set: ROWSEL_DIRTY says the screen
 * needs a repaint, ROWSEL_PLAYING says Now Playing was pushed over a track
 * that has just started (np_first, which skips the first partial paint).
 */
#define ROWSEL_DIRTY   1
#define ROWSEL_PLAYING 2

play_tap_t play_tap_start(fat32_t *fs);
int  rowsel_sub(void);
void rowsel_arm(int sel, keyhold_t *k, uint32_t now_us);
void rowsel_drop(keyhold_t *k);
int  row_select_tap(fat32_t *fs);
int  row_select_hold(fat32_t *fs);

#endif /* CORE_APP_ACTIONS_H */
