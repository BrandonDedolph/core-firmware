/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/nowplaying.h — the Now Playing screen: the full renderer, the
 * once-a-second transport band, the volume plate, and the gesture state the
 * band reads (the wheel scrubber, the SELECT press timer, the RIGHT/LEFT seek
 * holds). run_ui feeds that state and uses the timing constants below.
 * Moved verbatim out of kernel/main.c, plus the screen's input handler
 * (nowplaying_input: the wheel's volume/scrub, SELECT's down-edge, MENU).
 */

#ifndef CORE_APP_NOWPLAYING_H
#define CORE_APP_NOWPLAYING_H

#include <stdint.h>

#include "hw/clickwheel.h"
#include "../fs/fat32.h"
#include "../ui/gesture.h"
#include "ui_loop.h"

/*
 * Now-Playing scrub (seek) state.
 *
 * The decoders and the player have supported seeking all along
 * (player_seek_to); nothing was bound to it, which made long tracks,
 * podcasts and audiobooks effectively unusable.
 *
 * The wheel aims at a target rather than seeking on every detent: a seek stops
 * the DAC, re-primes the ring and on a backward jump rewinds the file, so doing
 * that per detent would stutter and hammer the disk. The target is committed
 * once the wheel goes quiet (SCRUB_COMMIT_US), which is also what makes a long
 * drag across a track cost exactly one seek.
 */
#define SCRUB_COMMIT_US  400000u     /* wheel quiet this long -> perform seek  */
#define SCRUB_EXIT_US   4000000u     /* ...and this long -> back to volume     */
#define SCRUB_STEP_S          5      /* seconds per detent at rest             */

/* SELECT press-length arbitration on Now Playing: tap = scrubber, hold = queue. */
#define SEL_HOLD_US 450000u
/* PLAY held this long sleeps the device; released sooner, it is play/pause. */
#define PLAY_HOLD_US 2000000u

extern uint32_t g_scrub_target_s;
extern uint32_t g_scrub_last_us;
extern int      g_scrub_dirty;
extern uint32_t g_sel_down_us;
extern int      g_sel_pending;
extern seekhold_t g_ff;
extern seekhold_t g_rw;

int  np_scrubbing(void);
void scrub_enter(void);
void scrub_exit(void);
void nowplaying_transport_render(uint32_t elapsed_s, uint32_t total_s);
void nowplaying_render(const char *name, uint32_t elapsed_s,
                       uint32_t total_s, uint32_t buf_pct);

/* SCR_NOWPLAYING's input: run_ui calls it with the drained event, its loop
 * state, the mounted volume and the event's tick while Now Playing is on
 * top. */
void nowplaying_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                      uint32_t ev_us);

#endif /* CORE_APP_NOWPLAYING_H */
