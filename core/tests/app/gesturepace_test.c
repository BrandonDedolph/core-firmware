/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/app/gesturepace_test.c — the timing decisions of run_ui's Now
 * Playing SELECT and wheel scrubber on the host.
 *
 * gesturepace.c is pure, so every case is one main-loop pass with exact
 * times: the caller's USEC_TIMER read and the stamp (SELECT's down-edge, the
 * scrubber's last wheel move). Thresholds are the firmware's: SEL_HOLD_US
 * 450 ms, SCRUB_COMMIT_US 400 ms, SCRUB_EXIT_US 4 s (app/nowplaying.h).
 * Pinned, at stamps either side of the 32-bit microsecond wrap (~71.6 min):
 *   1. SELECT: released is the TAP at any age, even 0 us and even past the
 *      hold; still down is WAIT up to a microsecond short of the hold and
 *      the HOLD from the hold on, to 2^32-1 us.
 *   2. Scrubber with a target aimed: WAIT until 400 ms of quiet, COMMIT from
 *      then on — never EXIT, however long the quiet.
 *   3. Scrubber with nothing aimed: WAIT until 4 s, EXIT from then on —
 *      never COMMIT, though 400 ms has passed.
 *   4. Ages of 2^31 us and beyond are long ago (an unsigned comparison).
 */

#include <stdio.h>
#include <stdint.h>

#include "gesturepace.h"

#define SEL_HOLD     450000u
#define SCR_COMMIT   400000u
#define SCR_EXIT    4000000u

static int g_fail;
static void check(const char *label, uint32_t now, uint32_t stamp, int flag,
                  int got, int want)
{
    if (got != want) {
        printf("FAIL: %s now=%u stamp=%u flag=%d: %d, want %d\n", label,
               (unsigned)now, (unsigned)stamp, flag, got, want);
        g_fail++;
    }
}

static const uint32_t g_ages_long[] = { 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
                                        0xFFFFFFFEu, 0xFFFFFFFFu };

static void sel_at(uint32_t d)
{
    static const uint32_t short_ages[] = { 0u, 1u, 1000u, SEL_HOLD - 1u };
    static const uint32_t hold_ages[]  = { SEL_HOLD, SEL_HOLD + 1u,
                                           2u * SEL_HOLD, 10000000u };
    for (unsigned i = 0; i < sizeof short_ages / sizeof short_ages[0]; i++) {
        uint32_t n = d + short_ages[i];
        check("sel down short", n, d, 1, npsel_decide(n, d, 1, SEL_HOLD),
              NPSEL_WAIT);
        check("sel up short", n, d, 0, npsel_decide(n, d, 0, SEL_HOLD),
              NPSEL_TAP);
    }
    for (unsigned i = 0; i < sizeof hold_ages / sizeof hold_ages[0]; i++) {
        uint32_t n = d + hold_ages[i];
        check("sel down hold", n, d, 1, npsel_decide(n, d, 1, SEL_HOLD),
              NPSEL_HOLD);
        check("sel up late", n, d, 0, npsel_decide(n, d, 0, SEL_HOLD),
              NPSEL_TAP);
    }
    for (unsigned i = 0; i < sizeof g_ages_long / sizeof g_ages_long[0]; i++) {
        uint32_t n = d + g_ages_long[i];
        check("sel down long", n, d, 1, npsel_decide(n, d, 1, SEL_HOLD),
              NPSEL_HOLD);
        check("sel up long", n, d, 0, npsel_decide(n, d, 0, SEL_HOLD),
              NPSEL_TAP);
    }
    /* Any `down` that is not 0 is down. */
    check("sel down=2", d + SEL_HOLD, d, 2,
          npsel_decide(d + SEL_HOLD, d, 2, SEL_HOLD), NPSEL_HOLD);
}

static scrubq_t sq(uint32_t n, uint32_t last, int dirty)
{
    return scrubq_decide(n, last, dirty, SCR_COMMIT, SCR_EXIT);
}

static void scrub_at(uint32_t last)
{
    /* Aimed. */
    static const uint32_t a_wait[] = { 0u, 1u, 1000u, SCR_COMMIT - 1u };
    static const uint32_t a_go[]   = { SCR_COMMIT, SCR_COMMIT + 1u,
                                       SCR_EXIT - 1u, SCR_EXIT, SCR_EXIT + 1u };
    for (unsigned i = 0; i < sizeof a_wait / sizeof a_wait[0]; i++)
        check("scrub aimed wait", last + a_wait[i], last, 1,
              sq(last + a_wait[i], last, 1), SCRUBQ_WAIT);
    for (unsigned i = 0; i < sizeof a_go / sizeof a_go[0]; i++)
        check("scrub aimed commit", last + a_go[i], last, 1,
              sq(last + a_go[i], last, 1), SCRUBQ_COMMIT);
    /* Nothing aimed. */
    static const uint32_t i_wait[] = { 0u, 1u, SCR_COMMIT - 1u, SCR_COMMIT,
                                       SCR_COMMIT + 1u, SCR_EXIT - 1u };
    static const uint32_t i_go[]   = { SCR_EXIT, SCR_EXIT + 1u, 2u * SCR_EXIT };
    for (unsigned i = 0; i < sizeof i_wait / sizeof i_wait[0]; i++)
        check("scrub idle wait", last + i_wait[i], last, 0,
              sq(last + i_wait[i], last, 0), SCRUBQ_WAIT);
    for (unsigned i = 0; i < sizeof i_go / sizeof i_go[0]; i++)
        check("scrub idle exit", last + i_go[i], last, 0,
              sq(last + i_go[i], last, 0), SCRUBQ_EXIT);
    for (unsigned i = 0; i < sizeof g_ages_long / sizeof g_ages_long[0]; i++) {
        uint32_t n = last + g_ages_long[i];
        check("scrub aimed long", n, last, 1, sq(n, last, 1), SCRUBQ_COMMIT);
        check("scrub idle long", n, last, 0, sq(n, last, 0), SCRUBQ_EXIT);
    }
    check("scrub dirty=2", last + SCR_COMMIT, last, 2,
          sq(last + SCR_COMMIT, last, 2), SCRUBQ_COMMIT);
}

int main(void)
{
    /* Stamps far from the wrap, and either side of it: a stamp just below
     * 2^32 makes every threshold land past 0. */
    static const uint32_t stamps[] = {
        0u, 1u, 123456789u, 0x7FFFFFFFu, 0x80000000u,
        0xFFFFFFFFu - SEL_HOLD, 0xFFFFFFFFu - SCR_COMMIT,
        0xFFFFFFFFu - SCR_EXIT, 0xFFFFFFFFu - 1u, 0xFFFFFFFFu,
    };
    for (unsigned i = 0; i < sizeof stamps / sizeof stamps[0]; i++) {
        sel_at(stamps[i]);
        scrub_at(stamps[i]);
    }
    /* The wrap itself, spelled out: down at 2^32-100, now 449,899 us later
     * (now = 449,799 after the wrap) is short; 100 us later it is the hold. */
    check("sel wrap short", 449799u, 0xFFFFFF9Cu, 1,
          npsel_decide(449799u, 0xFFFFFF9Cu, 1, SEL_HOLD), NPSEL_WAIT);
    check("sel wrap hold", 449900u, 0xFFFFFF9Cu, 1,
          npsel_decide(449900u, 0xFFFFFF9Cu, 1, SEL_HOLD), NPSEL_HOLD);
    check("scrub wrap wait", 399899u, 0xFFFFFF9Cu, 1,
          sq(399899u, 0xFFFFFF9Cu, 1), SCRUBQ_WAIT);
    check("scrub wrap commit", 399900u, 0xFFFFFF9Cu, 1,
          sq(399900u, 0xFFFFFF9Cu, 1), SCRUBQ_COMMIT);
    check("scrub wrap exit", 3999900u, 0xFFFFFF9Cu, 0,
          sq(3999900u, 0xFFFFFF9Cu, 0), SCRUBQ_EXIT);

    if (g_fail) {
        printf("gesturepace: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("gesturepace: OK\n");
    return 0;
}
