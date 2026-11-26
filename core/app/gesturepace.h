/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/gesturepace.h — the two timing decisions of run_ui's Now Playing
 * gestures, decided once per main-loop pass.
 *
 * Both used to be inline in run_ui's loop (kernel/main.c). The gestures they
 * decide for (gesture_np_select and gesture_np_scrub in app/gestures.c) keep
 * their USEC_TIMER read and pass it in with the stamp:
 *   - SELECT on Now Playing, while a press is pending: still down at
 *     `hold_us` or more after the down-edge is the HOLD (open the queue);
 *     released at any time before that is the TAP (toggle the scrubber);
 *     still down and short of it is WAIT.
 *   - The wheel scrubber, while it is up: a target aimed but not yet sought
 *     (`dirty`) is COMMITted once the wheel has been quiet `commit_us`; with
 *     nothing aimed, the scrubber lapses (EXIT) after `exit_us` of quiet;
 *     otherwise WAIT.
 * The thresholds are the callers' (SEL_HOLD_US, SCRUB_COMMIT_US and
 * SCRUB_EXIT_US in app/nowplaying.h).
 *
 * Pure and freestanding: no hardware access. `now` is the caller's
 * USEC_TIMER read and the stamp its own; the difference is taken as
 * uint32_t, so the ~71.6 min wrap is harmless. Host-tested in
 * tests/app/gesturepace_test.c.
 */
#ifndef CORE_APP_GESTUREPACE_H
#define CORE_APP_GESTUREPACE_H

#include <stdint.h>

typedef enum {
    NPSEL_WAIT = 0,     /* still down, short of the hold  */
    NPSEL_TAP,          /* released before the hold       */
    NPSEL_HOLD,         /* still down at the hold         */
} npsel_t;

/* SELECT pressed on Now Playing at `down_us`, `down` = still held now. */
npsel_t npsel_decide(uint32_t now, uint32_t down_us, int down,
                     uint32_t hold_us);

typedef enum {
    SCRUBQ_WAIT = 0,    /* keep scrubbing                 */
    SCRUBQ_COMMIT,      /* seek to the aimed target       */
    SCRUBQ_EXIT,        /* back to volume                 */
} scrubq_t;

/* The scrubber's wheel last moved at `last_us`; `dirty` = a target is aimed
 * and not yet sought. */
scrubq_t scrubq_decide(uint32_t now, uint32_t last_us, int dirty,
                       uint32_t commit_us, uint32_t exit_us);

#endif /* CORE_APP_GESTUREPACE_H */
