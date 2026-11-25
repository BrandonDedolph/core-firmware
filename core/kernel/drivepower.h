/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/drivepower.h — when the hard drive may be parked, and when it
 * must be held up.
 *
 * The policy used to live in two places: inline in run_ui's idle pass
 * (kernel/main.c) and inline in player_pump's burst-park branch
 * (player/player.c). Both halves are here now; the callers keep the hardware
 * (ata_standby(), ata_is_parked(), the timer reads) and pass the answers in.
 *
 * THE UI HALF (run_ui, once per main-loop pass, after the debounced commits
 * so a due save lands while the platters still turn):
 *   - Idle spin-down. Nothing playing, the drive spinning, no input for
 *     DISK_IDLE_US and no UI disk read (a cover batch) in the last
 *     UI_DISK_GRACE_US -> STANDBY. 10 s: long enough that reading a list and
 *     picking a song does not usually land on a spin-up (the next read after
 *     browsing is most often Play), short enough that a put-down device is
 *     not spinning for nothing. The grace stops a run of cover batches from
 *     spinning the drive up and down once per batch.
 *   - Lock park. The hold switch is the user saying they are done
 *     navigating: park at once rather than after the idle timeout, unless the
 *     player still needs the drive (player_disk_busy: a refill burst is
 *     reading, or, paused, the buffer is not yet stocked — the same test the
 *     paused pump stocks by, drivepower_paused_stocking below). main.c writes
 *     a pending setting and the On-The-Go list (CFG_COMMIT_SOFT) first, while
 *     the platters still turn: the idle path waits out a 3 s debounce and
 *     never wakes a parked drive, so a change made just before the lock would
 *     otherwise sit pending until the next spin-up.
 *   - A refused STANDBY is not re-issued every pass (each try can block for
 *     the ready wait): at most once per DISK_STANDBY_RETRY_US.
 *   - The browsing hold handed to the player (player_hold_drive): while
 *     playing, the player parks between refill bursts, and without the hold
 *     a menu opened mid-song read from a parked drive and froze for the
 *     spin-up. Held while the last input is no older than
 *     DISK_BROWSE_HOLD_US (5 s covers the gaps between presses of someone
 *     browsing) or a UI read is within its grace; never while locked
 *     (nobody is browsing, and the player's reads are never held back).
 *     Shorter than the idle spin-down because the music is what is in use
 *     now, and every second held is the drive's ~100 mA on top of it.
 *   - Park accounting: was each park worth it? A park followed by a read
 *     within DISK_PARK_QUICK_US paid a spin-up (power, and a wait if it was a
 *     menu) to save a few seconds of spinning; the idle timeout and the
 *     browsing hold are tuned by how often that happens in real use. Counted
 *     separately for parks made while playing (the player's, between bursts)
 *     and while not (the idle timeout, the hold switch), cumulative for the
 *     boot, in the 5 s `core: ui` line (`parks`/`quick`, `parks_play`/`quick`).
 *     The caller reads the timer only on a park edge (drivepower_park_edge),
 *     as it always has: in the headless build every timer read advances fake
 *     time.
 *
 * THE PLAYER HALF (player_pump, every pass): Apple-quiet playback parks the
 * drive between refill bursts. Once a burst has finished and the anti-skip
 * buffer is above its low mark, STANDBY — unless the UI's browsing hold is
 * on. The HAL's ata_is_parked() is checked as well as the pump's own flag: a
 * menu read spins the platters back up without telling the player, and keyed
 * on its own flag alone the pump left them spinning until its next burst.
 * After a STANDBY both flags are set and only a read clears the HAL's, so
 * this still issues one STANDBY per park. A STANDBY the drive refused leaves
 * the HAL flag clear; park_ok stops that being retried every pass until the
 * next burst. (The pump's own parked flag, g_drive_parked, stays in the
 * player: it also drives the spin-up probe and the parked refill gate.)
 *
 * THE OTHER PARK SITES, which stay where they are:
 *   - battery_refresh (kernel/main.c), the DISKSAFE shutdown: parks unless
 *     the player is mid-stream (active and not paused), after the last
 *     commits.
 *   - enter_standby (kernel/main.c): STANDBY before the PMU rail cut.
 *   - suspend_to_ram (kernel/main.c): ata_sleep() on the way down, with a
 *     STANDBY fallback, and the re-park after a commit woke the drive inside
 *     the suspend loop.
 *   - run_ui's cover pump (kernel/main.c) refuses to wake a parked platter
 *     while locked, or within CHIP_SPINUP_QUIET_US of a wheel turn; its loads
 *     are what drivepower_note_ui_read is told about.
 *   - player_resume (player/player.c) pre-pays the spin-up (ata_wakeup)
 *     before the DAC starts when the drive is parked and the buffer is short.
 *   - the paused pump (player/player.c) stocks the buffer up to DISK_LOW but
 *     never wakes a parked drive.
 *
 * Pure and freestanding: no hardware access; the caller passes the clock
 * (USEC_TIMER, wraps every ~71.6 min; all compares are differences).
 * Host-tested in tests/kernel/drivepower_test.c.
 */
#ifndef CORE_KERNEL_DRIVEPOWER_H
#define CORE_KERNEL_DRIVEPOWER_H

#include <stdint.h>

/* The drive's two timeouts after the last input, tuned from the park log
 * (`parks`/`quick` in the core: ui line): nothing playing, the idle spin-down;
 * playing, how long the player is held off parking between bursts. */
#define DISK_IDLE_US           10000000u
#define DISK_BROWSE_HOLD_US     5000000u
/* The last UI disk read (a cover batch), and how long after it the drive is
 * still kept up. */
#define UI_DISK_GRACE_US       5000000u
/* A refused STANDBY: retried at most this often. */
#define DISK_STANDBY_RETRY_US  10000000u
/* A park undone by a read within this long counts as `quick`. */
#define DISK_PARK_QUICK_US     15000000u

/* ---- the UI half ---------------------------------------------------------- */

/* Caller-owned; zero-initialised (a static) is the boot state. */
typedef struct {
    uint32_t ui_disk_us;               /* the last UI disk read                   */
    int      standby_failed;           /* the last idle/lock STANDBY was refused  */
    uint32_t standby_fail_us;          /* ...issued at                            */
    uint32_t park_n[2], park_quick[2]; /* [0] not playing, [1] playing            */
    uint32_t park_at_us;               /* the last park edge                      */
    int      park_was, park_kind;      /* parked as of the last edge; its [kind]  */
} drivepower_t;

/* One idle pass's view of the world. */
typedef struct {
    uint32_t now_us;      /* this pass's timer read                              */
    uint32_t idle_us;     /* since the last input                                */
    int      locked;      /* the hold switch                                     */
    int      playing;     /* player_playing(): the DAC running                   */
    int      parked;      /* ata_is_parked()                                     */
    int      player_busy; /* player_disk_busy(): the player still needs the drive */
} drivepower_in_t;

typedef struct {
    int standby;          /* issue STANDBY now, then drivepower_standby_result   */
    int flush_first;      /* ...a lock park: SOFT-commit settings and OTG before */
    int hold;             /* player_hold_drive(hold), every pass                 */
} drivepower_out_t;

/* A UI disk read (a cover batch) landed at `now_us`. */
void drivepower_note_ui_read(drivepower_t *dp, uint32_t now_us);

/* The idle pass's decision. Changes nothing; report a STANDBY's outcome
 * with drivepower_standby_result. */
drivepower_out_t drivepower_idle_feed(const drivepower_t *dp,
                                      const drivepower_in_t *in);

/* The STANDBY drivepower_idle_feed asked for returned `rc` (0 = parked);
 * `now_us` is the same pass's stamp. */
void drivepower_standby_result(drivepower_t *dp, int rc, uint32_t now_us);

/* 1 when `parked` differs from the last edge seen: only then does the
 * caller read the timer and call drivepower_park_note. */
int  drivepower_park_edge(const drivepower_t *dp, int parked);
void drivepower_park_note(drivepower_t *dp, int parked, int playing,
                          uint32_t now_us);

/* Cumulative park counts for the `core: ui` line; `playing` picks [1]. */
uint32_t drivepower_parks(const drivepower_t *dp, int playing);
uint32_t drivepower_quick_parks(const drivepower_t *dp, int playing);

/* ---- the player half ------------------------------------------------------ */

/* Caller-owned (the player); zero-initialised is the boot state. */
typedef struct {
    int hold;             /* 1 while the UI is in use: do not park  */
    int park_ok;          /* the pump's last STANDBY succeeded      */
} drivepower_burst_t;

void drivepower_burst_hold(drivepower_burst_t *b, int hold);

/* The pump is between bursts (not filling): may it park? `own_parked` is the
 * pump's own flag, `hal_parked` ata_is_parked(), `ahead` the bytes buffered
 * ahead of the decoder and `low` the refill mark. */
int  drivepower_burst_may_park(const drivepower_burst_t *b, int own_parked,
                               int hal_parked, uint32_t ahead, uint32_t low);

/* The burst park's STANDBY returned `rc` (0 = parked). */
void drivepower_burst_parked(drivepower_burst_t *b, int rc);

/* Paused, does the player still need the drive to stock its buffer to `low`
 * (no end of file, no read error)? The test the paused pump stocks by (the
 * pump also refuses a parked drive), as the second half of player_disk_busy. */
int  drivepower_paused_stocking(int paused, int eos, int err, uint32_t ahead,
                                uint32_t low);

#endif /* CORE_KERNEL_DRIVEPOWER_H */
