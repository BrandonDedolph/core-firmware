/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/app/animpace_test.c — the rate limits of run_ui's in-place
 * animations on the host.
 *
 * animpace.c is pure, so every case is one main-loop pass with exact times:
 * the caller's USEC_TIMER read and the stamp of the last step. Pinned:
 *   1. The now-playing bars step at 110 ms and not a microsecond before; the
 *      marquee at 33 ms.
 *   2. A step just taken (now == last) is not due again.
 *   3. 1-2 across the 32-bit microsecond wrap (~71.6 min): a stamp just
 *      below 2^32 and a now just above 0.
 *   4. Ages of 2^31 us (~35.8 min) up to 0xFFFFFFFF are long ago (an
 *      unsigned comparison): due.
 */

#include <stdio.h>
#include <stdint.h>

#include "animpace.h"

static int g_fail;
static void check(const char *label, uint32_t now, uint32_t last, int got,
                  int want)
{
    if (got != want) {
        printf("FAIL: %s now=%u last=%u: %d, want %d\n", label,
               (unsigned)now, (unsigned)last, got, want);
        g_fail++;
    }
}

typedef int (*due_fn)(uint32_t now, uint32_t last);

/* One rate limit of period `p`, with the last step at `last`. */
static void limit_at(const char *label, due_fn due, uint32_t p, uint32_t last)
{
    static const uint32_t before[] = { 0u, 1u, 2u, 1000u };
    for (unsigned i = 0; i < sizeof before / sizeof before[0]; i++) {
        uint32_t age = before[i] < p ? before[i] : 0u;
        check(label, last + age, last, due(last + age, last), 0);
    }
    check(label, last + p - 1u, last, due(last + p - 1u, last), 0);
    check(label, last + p, last, due(last + p, last), 1);
    check(label, last + p + 1u, last, due(last + p + 1u, last), 1);
    check(label, last + 2u * p, last, due(last + 2u * p, last), 1);
    /* Long ago: 2^31 and beyond is still a positive age. */
    static const uint32_t old[] = { 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
                                    0xFFFFFFFEu, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof old / sizeof old[0]; i++)
        check(label, last + old[i], last, due(last + old[i], last), 1);
}

static void both_at(uint32_t last)
{
    limit_at("bars", bars_step_due, 110000u, last);
    limit_at("marquee", marquee_step_due, 33000u, last);
}

int main(void)
{
    /* The periods are what the comments in run_ui promise. */
    if (BARS_STEP_US != 110000u || MQ_STEP_US != 33000u) {
        printf("FAIL: periods %u %u\n", (unsigned)BARS_STEP_US,
               (unsigned)MQ_STEP_US);
        g_fail++;
    }

    /* run_ui starts both stamps at 0. */
    both_at(0u);
    both_at(1u);
    both_at(5000000u);
    /* Across the wrap: the step lands on the far side of 2^32. */
    both_at(0xFFFFFFFFu);
    both_at(0xFFFFFFFFu - 32999u);
    both_at(0xFFFFFFFFu - 109999u);
    both_at(0xFFFFFFFFu - 50000u);
    both_at(0x7FFFFFFFu);
    both_at(0x80000000u);

    /* Spot checks with literal numbers, so a broken limit_at cannot hide a
     * broken policy. */
    check("bars wrap", 0x00000010u, 0xFFFE5260u,
          bars_step_due(0x00000010u, 0xFFFE5260u), 1);   /* 110000 us */
    check("bars wrap", 0x00000010u, 0xFFFE5261u,
          bars_step_due(0x00000010u, 0xFFFE5261u), 0);   /* 109999 us */
    check("marquee wrap", 0x00000000u, 0xFFFF7F18u,
          marquee_step_due(0x00000000u, 0xFFFF7F18u), 1); /* 33000 us */
    check("marquee wrap", 0x00000000u, 0xFFFF7F19u,
          marquee_step_due(0x00000000u, 0xFFFF7F19u), 0); /* 32999 us */

    if (g_fail) {
        printf("animpace: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("animpace: ok\n");
    return 0;
}
