/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/app/coverpump_test.c — the album list's per-pass cover-load budget
 * on the host.
 *
 * coverpump.c is pure, so every case is one main-loop pass with exact
 * times. The times are built the way albumlist_pump_chips builds them (its
 * USEC_TIMER read minus a stamp, as uint32_t), so the wrap cases go through
 * the same arithmetic. Pinned:
 *   1. Parked and locked: nothing, whatever the wheel, the player or the
 *      rate limit, and the pass is NOT counted as held.
 *   2. Parked, wheel moved within CHIP_SPINUP_QUIET_US: nothing, counted as
 *      held. From QUIET on, a parked drive gets the settled budget.
 *   3. Spinning (or parked but quiet), wheel moved within
 *      CHIP_WHEEL_SETTLE_US: one read. Locked does not matter unless parked.
 *   4. Settled: six reads idle, three with a song loaded.
 *   5. The 60 ms rate limit applies only with a song loaded; idle passes are
 *      never limited.
 *   6. 1-5 across the 32-bit microsecond wrap.
 */

#include <stdio.h>
#include <stdint.h>

#include "coverpump.h"

static int g_fail;
static int check(const char *label, int cond)
{
    if (!cond) {
        printf("FAIL: %s\n", label);
        g_fail++;
    }
    return cond;
}

/* One pass: the caller's now, the wheel's stamp, the last batch's stamp. */
static int pass(int parked, int locked, uint32_t now, uint32_t wheel_us,
                int idle, uint32_t last_chip, int *held)
{
    *held = -1;   /* the policy must always write it */
    return coverpump_budget(parked, locked, now - wheel_us, idle,
                            (uint32_t)(now - last_chip), held);
}

static void expect(const char *label, int parked, int locked, uint32_t now,
                   uint32_t wheel_us, int idle, uint32_t last_chip,
                   int want_budget, int want_held)
{
    int held;
    int b = pass(parked, locked, now, wheel_us, idle, last_chip, &held);
    if (!check(label, b == want_budget && held == want_held))
        printf("      budget %d held %d, want %d %d\n", b, held,
               want_budget, want_held);
}

static void cases_at(uint32_t t0, const char *tag)
{
    char l[128];
    const uint32_t S = CHIP_WHEEL_SETTLE_US, Q = CHIP_SPINUP_QUIET_US;
    const uint32_t long_ago = t0 - 10000000u;   /* 10 s: settled, unlimited */

    /* 1. parked and locked */
    for (int idle = 0; idle < 2; idle++) {
        uint32_t wheels[] = { t0, t0 - 1, t0 - S, t0 - Q + 1, t0 - Q,
                              long_ago };
        for (unsigned i = 0; i < sizeof wheels / sizeof *wheels; i++) {
            snprintf(l, sizeof l, "%s parked+locked idle=%d wheel#%u: nothing",
                     tag, idle, i);
            expect(l, 1, 1, t0, wheels[i], idle, long_ago, 0, 0);
            snprintf(l, sizeof l, "%s parked+locked idle=%d wheel#%u, batch just now",
                     tag, idle, i);
            expect(l, 1, 1, t0, wheels[i], idle, t0, 0, 0);
        }
    }

    /* 2. parked, wheel recently moved: held; quiet: settled budget */
    for (int idle = 0; idle < 2; idle++) {
        snprintf(l, sizeof l, "%s parked, wheel this us, idle=%d: held", tag, idle);
        expect(l, 1, 0, t0, t0, idle, long_ago, 0, 1);
        snprintf(l, sizeof l, "%s parked, wheel SETTLE ago, idle=%d: held", tag, idle);
        expect(l, 1, 0, t0, t0 - S, idle, long_ago, 0, 1);
        snprintf(l, sizeof l, "%s parked, wheel QUIET-1 ago, idle=%d: held", tag, idle);
        expect(l, 1, 0, t0, t0 - (Q - 1), idle, long_ago, 0, 1);
        snprintf(l, sizeof l, "%s parked, wheel QUIET ago, idle=%d: settled", tag, idle);
        expect(l, 1, 0, t0, t0 - Q, idle, long_ago, idle ? 6 : 3, 0);
        snprintf(l, sizeof l, "%s parked, wheel 10 s ago, idle=%d: settled", tag, idle);
        expect(l, 1, 0, t0, long_ago, idle, long_ago, idle ? 6 : 3, 0);
        /* held even when the rate limit would have refused anyway */
        snprintf(l, sizeof l, "%s parked, wheel moving, batch just now, idle=%d: held",
                 tag, idle);
        expect(l, 1, 0, t0, t0 - 1, idle, t0, 0, 1);
    }

    /* 3. moving with the platter up: one read; locked is irrelevant */
    for (int idle = 0; idle < 2; idle++)
        for (int locked = 0; locked < 2; locked++) {
            snprintf(l, sizeof l, "%s spinning locked=%d idle=%d, wheel now: 1",
                     tag, locked, idle);
            expect(l, 0, locked, t0, t0, idle, long_ago, 1, 0);
            snprintf(l, sizeof l, "%s spinning locked=%d idle=%d, wheel SETTLE-1: 1",
                     tag, locked, idle);
            expect(l, 0, locked, t0, t0 - (S - 1), idle, long_ago, 1, 0);
            snprintf(l, sizeof l, "%s spinning locked=%d idle=%d, wheel SETTLE: settled",
                     tag, locked, idle);
            expect(l, 0, locked, t0, t0 - S, idle, long_ago, idle ? 6 : 3, 0);
        }

    /* 4. settled */
    snprintf(l, sizeof l, "%s settled idle: 6", tag);
    expect(l, 0, 0, t0, long_ago, 1, long_ago, 6, 0);
    snprintf(l, sizeof l, "%s settled playing: 3", tag);
    expect(l, 0, 0, t0, long_ago, 0, long_ago, 3, 0);

    /* 5. the 60 ms rate limit: with a song loaded only */
    snprintf(l, sizeof l, "%s playing, batch 59999 us ago: wait", tag);
    expect(l, 0, 0, t0, long_ago, 0, t0 - 59999u, 0, 0);
    snprintf(l, sizeof l, "%s playing, batch 60000 us ago: 3", tag);
    expect(l, 0, 0, t0, long_ago, 0, t0 - 60000u, 3, 0);
    snprintf(l, sizeof l, "%s playing+moving, batch 59999 us ago: wait", tag);
    expect(l, 0, 0, t0, t0 - 1, 0, t0 - 59999u, 0, 0);
    snprintf(l, sizeof l, "%s playing+moving, batch 60000 us ago: 1", tag);
    expect(l, 0, 0, t0, t0 - 1, 0, t0 - 60000u, 1, 0);
    snprintf(l, sizeof l, "%s idle, batch this us: 6 (no limit)", tag);
    expect(l, 0, 0, t0, long_ago, 1, t0, 6, 0);
    snprintf(l, sizeof l, "%s idle+moving, batch this us: 1 (no limit)", tag);
    expect(l, 0, 0, t0, t0, 1, t0, 1, 0);
}

int main(void)
{
    /* Mid-range, then with every stamp on the far side of the wrap. */
    cases_at(0x40000000u, "mid");
    cases_at(0x00000010u, "wrap");
    cases_at(0xFFFFFFF0u, "pre-wrap");

    /* 6. explicit wrap: the wheel stamped just before the wrap, now just
     * after it — 0x20 us of real time, so moving, not 71 minutes. */
    {
        int held;
        int b = pass(0, 0, 0x10u, 0xFFFFFFF0u, 1, 0xFFFFFFF0u, &held);
        check("wrap: wheel 0x20 us ago across the wrap is moving", b == 1 && held == 0);
        b = pass(1, 0, 0x10u, 0xFFFFFFF0u, 1, 0, &held);
        check("wrap: parked, wheel 0x20 us ago across the wrap is held", b == 0 && held == 1);
        b = pass(0, 0, 0x10u, 0x10u - 1000000u, 0, 0xFFFFFFF0u, &held);
        check("wrap: playing, batch 0x20 us ago across the wrap waits", b == 0 && held == 0);
        b = pass(0, 0, 0x10u, 0x10u - 1000000u, 0, 0x10u - 60000u, &held);
        check("wrap: playing, batch 60 ms ago across the wrap loads 3", b == 3 && held == 0);
    }

    if (g_fail) {
        printf("coverpump: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("coverpump: all cases pass\n");
    return 0;
}
