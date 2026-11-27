/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/app/presentpace_test.c — the pacing of run_ui's UI presents on the
 * host.
 *
 * presentpace.c is pure, so every case is exact numbers: the last present's
 * measured cost, the caller's USEC_TIMER read and the last present's stamp.
 * Pinned:
 *   1. The gap is 4x the last present's cost, never under 20 ms and never
 *      over 150 ms, at each clamp's boundary microsecond; the boot seed
 *      (30 ms) paces at 120 ms.
 *   2. The multiply is uint32_t: a cost of 2^30 us or more wraps before the
 *      clamp, as the inline code always did.
 *   3. A present is due once the gap has passed and not a microsecond
 *      before; one just made (now == last) is not due again unless the gap
 *      is 0.
 *   4. 3 holds across the 32-bit microsecond wrap (~71.6 min), and ages from 2^31
 *      up to 0xFFFFFFFF are long ago (an unsigned comparison): due.
 */

#include <stdio.h>
#include <stdint.h>

#include "presentpace.h"

static int g_fail;

static void gap_is(uint32_t cost, uint32_t want)
{
    uint32_t got = present_gap_for(cost);
    if (got != want) {
        printf("FAIL: present_gap_for(%u) = %u, want %u\n", (unsigned)cost,
               (unsigned)got, (unsigned)want);
        g_fail++;
    }
}

static void due_is(const char *label, uint32_t now, uint32_t last,
                   uint32_t gap, int want)
{
    int got = present_due(now, last, gap);
    if (got != want) {
        printf("FAIL: %s present_due(now=%u, last=%u, gap=%u) = %d, want %d\n",
               label, (unsigned)now, (unsigned)last, (unsigned)gap, got, want);
        g_fail++;
    }
}

/* One gap, with the last present at `last`. */
static void due_at(uint32_t gap, uint32_t last)
{
    static const uint32_t before[] = { 0u, 1u, 2u, 1000u };
    for (unsigned i = 0; i < sizeof before / sizeof before[0]; i++) {
        if (before[i] >= gap) continue;
        due_is("early", last + before[i], last, gap, 0);
    }
    if (gap) due_is("gap-1", last + gap - 1u, last, gap, 0);
    due_is("gap", last + gap, last, gap, 1);
    due_is("gap+1", last + gap + 1u, last, gap, 1);
    due_is("2gap", last + 2u * gap, last, gap, 1);
    /* Long ago: 2^31 and beyond is still a positive age. */
    static const uint32_t old[] = { 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
                                    0xFFFFFFFEu, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof old / sizeof old[0]; i++)
        due_is("old", last + old[i], last, gap, 1);
}

int main(void)
{
    /* The constants are what present_gap_us's comment promises. */
    if (PRESENT_GAP_MULT != 4u || PRESENT_GAP_MIN_US != 20000u ||
        PRESENT_GAP_MAX_US != 150000u) {
        printf("FAIL: constants %u %u %u\n", (unsigned)PRESENT_GAP_MULT,
               (unsigned)PRESENT_GAP_MIN_US, (unsigned)PRESENT_GAP_MAX_US);
        g_fail++;
    }

    /* 1. The clamp, at both boundaries. */
    gap_is(0u, 20000u);
    gap_is(1u, 20000u);
    gap_is(4999u, 20000u);        /* 19996 */
    gap_is(5000u, 20000u);        /* exactly the floor */
    gap_is(5001u, 20004u);
    gap_is(10800u, 43200u);       /* a full frame, device log 2026-09-13 */
    gap_is(30000u, 120000u);      /* g_present_cost_us's boot seed */
    gap_is(37499u, 149996u);
    gap_is(37500u, 150000u);      /* exactly the ceiling */
    gap_is(37501u, 150000u);      /* 150004 */
    gap_is(1000000u, 150000u);
    gap_is(0x3FFFFFFFu, 150000u); /* 0xFFFFFFFC */
    /* 2. The multiply wraps first. */
    gap_is(0x40000000u, 20000u);          /* 4 * 2^30 = 0      */
    gap_is(0x40000000u + 6000u, 24000u);  /* wraps to 24000    */
    gap_is(0x40000000u + 37501u, 150000u);
    gap_is(0xFFFFFFFFu, 150000u);         /* 0xFFFFFFFC        */
    /* Every cost up to past the ceiling, against the formula. */
    for (uint32_t c = 0; c <= 40000u; c++) {
        uint32_t want = c * 4u;
        if (want < 20000u) want = 20000u;
        if (want > 150000u) want = 150000u;
        if (present_gap_for(c) != want) { gap_is(c, want); break; }
    }

    /* 3-4. The comparison, at the gaps present_gap_for can return. run_ui
     * starts L->last_present at 0. */
    static const uint32_t gaps[] = { 0u, 1u, 20000u, 43200u, 120000u, 150000u };
    static const uint32_t lasts[] = { 0u, 1u, 5000000u, 0x7FFFFFFFu,
                                      0x80000000u, 0xFFFFFFFFu,
                                      0xFFFFFFFFu - 19999u,
                                      0xFFFFFFFFu - 149999u,
                                      0xFFFFFFFFu - 50000u };
    for (unsigned g = 0; g < sizeof gaps / sizeof gaps[0]; g++)
        for (unsigned l = 0; l < sizeof lasts / sizeof lasts[0]; l++)
            due_at(gaps[g], lasts[l]);

    /* Spot checks with literal numbers, so a broken due_at cannot hide a
     * broken policy. */
    due_is("wrap", 0x00000010u, 0xFFFDB620u, 150000u, 1);   /* 150000 us */
    due_is("wrap", 0x00000010u, 0xFFFDB621u, 150000u, 0);   /* 149999 us */
    due_is("wrap", 0x00000000u, 0xFFFFB1E0u, 20000u, 1);    /* 20000 us  */
    due_is("wrap", 0x00000000u, 0xFFFFB1E1u, 20000u, 0);    /* 19999 us  */
    /* The two together, as render_tick uses them: the seed paces 120 ms. */
    due_is("seed", 119999u, 0u, present_gap_for(30000u), 0);
    due_is("seed", 120000u, 0u, present_gap_for(30000u), 1);

    if (g_fail) {
        printf("presentpace: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("presentpace: ok\n");
    return 0;
}
