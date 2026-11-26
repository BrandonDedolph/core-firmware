/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/coverpump.h — how many album-cover chips the album list may load
 * this main-loop pass.
 *
 * The rule used to be inline in run_ui's loop (kernel/main.c). It is here
 * now; the disk and cache work it gates (albumlist_pump_chips in
 * app/albums.c) keeps the timer reads, the artcache calls and the counters
 * in the same order, and passes the answers in.
 *
 * Every cover load is a SYNCHRONOUS disk read on the UI loop, so:
 *   - Parked and locked: nothing. The lock parks the drive, and the user has
 *     said they are done with the menus; waking it for a cover was a spin-up
 *     and a park per batch.
 *   - Parked, and the wheel moved within CHIP_SPINUP_QUIET_US: nothing, and
 *     the pass counts as "held" (g_ui_art_held). The spin-up would freeze the
 *     loop for 1-3 s mid-gesture; it waits for a still screen instead.
 *   - Otherwise, the wheel moved within CHIP_WHEEL_SETTLE_US (scrolling):
 *     one read.
 *   - Otherwise (settled): 6 reads idle, 3 while a song is loaded.
 *   - Whatever the budget, a pass with a song loaded loads only when 60 ms
 *     have passed since the last batch; idle passes are not rate-limited.
 *
 * Pure and freestanding: no hardware access. Both times are the caller's
 * uint32_t differences (its USEC_TIMER read minus the wheel stamp, minus the
 * last batch's stamp), so the ~71.6 min wrap is harmless. Host-tested in
 * tests/app/coverpump_test.c.
 */
#ifndef CORE_APP_COVERPUMP_H
#define CORE_APP_COVERPUMP_H

#include <stdint.h>

/* Cover loads under a moving wheel (albumlist_pump_chips in app/albums.c; see the
 * block comment there). A detent within SETTLE means the list is scrolling:
 * one disk read per pass, not six. A parked platter is only woken for covers
 * after QUIET of wheel silence — long enough that a deliberate detent-by-
 * detent scroll (~300 ms apart) never trips a spin-up mid-gesture. */
#define CHIP_WHEEL_SETTLE_US   150000u
#define CHIP_SPINUP_QUIET_US   500000u

/* The number of chips this pass may load; 0 means do not pump at all this
 * pass. `parked` is ata_is_parked(), `locked` g_locked, `since_wheel` the
 * time since the last wheel detent, `idle` !player_active(), `since_chip`
 * the time since the last batch. *held is set to 1 when the parked-and-
 * recently-moved rule held covers back (the caller counts it), else 0. */
int coverpump_budget(int parked, int locked, uint32_t since_wheel,
                     int idle, uint32_t since_chip, int *held);

#endif /* CORE_APP_COVERPUMP_H */
