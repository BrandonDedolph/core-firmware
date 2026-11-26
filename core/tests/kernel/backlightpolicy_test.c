/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/backlightpolicy_test.c — the idle backlight and the idle CPU
 * clock on the host.
 *
 * backlightpolicy.c is pure, so every case is a main-loop pass with an exact
 * idle time. The scenarios drive a small model of run_ui's side of the seam
 * (`ui_t` below: the LED level, bl_state, cpu_idled and the boost refcount,
 * applied exactly as run_ui applies the answers), so what is pinned is what
 * the device does, pass by pass. Pinned:
 *   1. Dim strictly after the Settings timeout, at bright/4 (never below 1);
 *      off strictly after timeout + 15 s; one step per pass.
 *   2. "Never" (0, and anything below): no dim, no off, whatever the idle;
 *      a screen already dimmed when "never" is chosen stays dimmed.
 *   3. The policy never relights: only an input does (the input sites set
 *      BL_FULL and restamp), and the next dim is a whole timeout after it.
 *   4. Wake from DIM and from OFF on input, and the CPU back up that pass.
 *   5. Playback: the screen dims and goes out as usual; the core is not
 *      idled while playing, is idled when it stops, re-boosted when it
 *      starts. External power and the Hold switch are not inputs to the
 *      policy at all (the screen times out on the cable and under Hold
 *      alike): there is nothing to pass, so nothing to pin but that.
 *   6. The cpu_idled balance: over random passes, inputs, play toggles,
 *      setting changes and suspends, every unboost is paired with exactly
 *      one boost and the refcount is always 1 (lit or playing) or 0 (idled).
 *      The suspend's own wake boost needs the caller to clear cpu_idled
 *      (run_ui does): shown both ways.
 *   7. 1-4 across the 32-bit microsecond wrap (idle is now - last_input,
 *      as uint32_t, exactly as run_ui computes it).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backlightpolicy.h"

static int g_fail;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) g_fail = 1;
    return cond;
}

#define S(n)   ((uint32_t)(n) * 1000000u)      /* seconds -> us */

static char g_lbl[128];
static const char *L(const char *tag, const char *what)
{
    snprintf(g_lbl, sizeof g_lbl, "%s %s", tag, what);
    return g_lbl;
}

/* run_ui's side of the seam. */
typedef struct {
    uint32_t last_input;
    int      bl_state;
    int      cpu_idled;
    int      boost;            /* the cpu_boost refcount                */
    int      led;              /* the last backlight_set()              */
    int      led_sets;         /* backlight_set() calls                 */
    int      boosts, unboosts; /* cpu_boost / cpu_unboost from the loop */
    int      susp_restores;    /* idled, restored by a suspend's wake   */
    int      bad;              /* an unboost at 0, or a refcount > 1    */
    int      secs, bright, playing;
} ui_t;

static void boot(ui_t *d, uint32_t now, int secs, int bright)
{
    memset(d, 0, sizeof *d);
    d->last_input = now;           /* ui_boot's stamp                 */
    d->bl_state   = BL_FULL;
    d->boost      = 1;             /* the boot path's boost           */
    d->led        = bright;
    d->secs       = secs;
    d->bright     = bright;
}

static void led_set(ui_t *d, int v) { d->led = v; d->led_sets++; }

/* One loop pass's backlight + CPU block, as run_ui applies it. */
static void pass(ui_t *d, uint32_t now)
{
    uint32_t idle = now - d->last_input;
    int bl_step = backlightpolicy_step(d->bl_state, idle, d->secs);
    if (bl_step == BLP_DIM) {
        led_set(d, backlightpolicy_dim_level(d->bright));
        d->bl_state = BL_DIM;
    } else if (bl_step == BLP_OFF) {
        led_set(d, 0);
        d->bl_state = BL_OFF;
    }
    int cpu_step = backlightpolicy_cpu(d->cpu_idled, d->bl_state, d->playing);
    if (cpu_step == BLP_CPU_BOOST) {
        d->boost++;
        d->boosts++;
        d->cpu_idled = 0;
    } else if (cpu_step == BLP_CPU_IDLE) {
        if (d->boost == 0) d->bad = 1;
        d->boost--;
        d->unboosts++;
        d->cpu_idled = 1;
    }
    if (d->boost > 1 || d->boost < 0) d->bad = 1;
}

/* An input site (the Hold edge, a button under Hold, any wheel event). */
static void input(ui_t *d, uint32_t now)
{
    d->last_input = now;
    if (d->bl_state != BL_FULL) {
        led_set(d, d->bright);
        d->bl_state = BL_FULL;
    }
}

/* suspend_to_ram and back: its wake re-establishes ONE boost; run_ui then
 * restamps, relights and (clear_idled) clears cpu_idled. */
static void suspend(ui_t *d, uint32_t now, int clear_idled)
{
    if (d->cpu_idled && clear_idled) d->susp_restores++;
    d->boost      = 1;
    d->led        = d->bright;
    d->last_input = now;
    d->bl_state   = BL_FULL;
    if (clear_idled) d->cpu_idled = 0;
}

static void test_steps(void)
{
    /* The step function alone, at every boundary. */
    check("step: lit, idle == timeout -> stay",
          backlightpolicy_step(BL_FULL, S(15), 15) == BLP_STAY);
    check("step: lit, idle == timeout + 1 us -> dim",
          backlightpolicy_step(BL_FULL, S(15) + 1u, 15) == BLP_DIM);
    check("step: dimmed, idle == timeout + 15 s -> stay",
          backlightpolicy_step(BL_DIM, S(30), 15) == BLP_STAY);
    check("step: dimmed, idle == timeout + 15 s + 1 us -> off",
          backlightpolicy_step(BL_DIM, S(30) + 1u, 15) == BLP_OFF);
    check("step: lit, idle far past off -> dim only (one step a pass)",
          backlightpolicy_step(BL_FULL, S(3600), 15) == BLP_DIM);
    check("step: off stays off", backlightpolicy_step(BL_OFF, S(3600), 15) == BLP_STAY);
    check("step: off stays off at idle 0", backlightpolicy_step(BL_OFF, 0, 15) == BLP_STAY);
    check("step: dimmed, idle back to 0 -> stay (the policy never relights)",
          backlightpolicy_step(BL_DIM, 0, 15) == BLP_STAY);
    check("step: 60 s -> off at 75 s",
          backlightpolicy_step(BL_DIM, S(75), 60) == BLP_STAY &&
          backlightpolicy_step(BL_DIM, S(75) + 1u, 60) == BLP_OFF);
    check("step: 5 s -> dim at 5 s, off at 20 s",
          backlightpolicy_step(BL_FULL, S(5) + 1u, 5) == BLP_DIM &&
          backlightpolicy_step(BL_DIM, S(20) + 1u, 5) == BLP_OFF);

    /* "Never". */
    static const int never[] = { 0, -1, -60 };
    for (unsigned i = 0; i < sizeof never / sizeof never[0]; i++) {
        int ok = backlightpolicy_step(BL_FULL, 0xFFFFFFFFu, never[i]) == BLP_STAY &&
                 backlightpolicy_step(BL_DIM,  0xFFFFFFFFu, never[i]) == BLP_STAY &&
                 backlightpolicy_step(BL_FULL, S(16), never[i]) == BLP_STAY;
        check("step: never (<= 0) never dims, never goes off", ok);
    }

    /* The dimmed level. */
    check("dim level: 32 -> 8", backlightpolicy_dim_level(32) == 8);
    check("dim level: 7 -> 1",  backlightpolicy_dim_level(7) == 1);
    check("dim level: 8 -> 2",  backlightpolicy_dim_level(8) == 2);
    check("dim level: 1..3 -> 1 (floor)",
          backlightpolicy_dim_level(1) == 1 && backlightpolicy_dim_level(3) == 1);
    check("dim level: 0 and below -> 1",
          backlightpolicy_dim_level(0) == 1 && backlightpolicy_dim_level(-8) == 1);

    /* The CPU step, the full table. */
    int ok = 1;
    for (int idled = 0; idled <= 1; idled++)
        for (int bl = BL_OFF; bl <= BL_FULL; bl++)
            for (int play = 0; play <= 1; play++) {
                int want = BLP_CPU_STAY;
                if (idled && (bl != BL_OFF || play)) want = BLP_CPU_BOOST;
                if (!idled && bl == BL_OFF && !play) want = BLP_CPU_IDLE;
                if (backlightpolicy_cpu(idled, bl, play) != want) ok = 0;
            }
    check("cpu: idle only dark and silent, boost on any life, else stay", ok);
    check("cpu: idled and dark and silent -> stay (no second unboost)",
          backlightpolicy_cpu(1, BL_OFF, 0) == BLP_CPU_STAY);
    check("cpu: not idled and lit -> stay (no second boost)",
          backlightpolicy_cpu(0, BL_FULL, 0) == BLP_CPU_STAY);
    check("cpu: dimmed is not dark: no idle",
          backlightpolicy_cpu(0, BL_DIM, 0) == BLP_CPU_STAY);
}

/* 1, 3, 4, 5 as passes on the model, from `base`. */
static void test_timeline(uint32_t base, const char *tag)
{
    ui_t d;
    uint32_t t;

    /* 1. Dim then off, 15 s setting, 32 brightness, a pass every 10 ms. */
    boot(&d, base, 15, 32);
    for (t = base; t != base + S(15); t += 10000u) pass(&d, t);
    pass(&d, base + S(15));
    check(L(tag, "lit through the timeout"), d.bl_state == BL_FULL && d.led == 32 &&
          d.led_sets == 0);
    pass(&d, base + S(15) + 1u);
    check(L(tag, "dimmed 1 us after it, at 8"), d.bl_state == BL_DIM && d.led == 8 &&
          d.led_sets == 1);
    check(L(tag, "dimmed: the core stays up"), !d.cpu_idled && d.boost == 1);
    pass(&d, base + S(30));
    check(L(tag, "dimmed through timeout + 15 s"), d.bl_state == BL_DIM);
    pass(&d, base + S(30) + 1u);
    check(L(tag, "off 1 us after timeout + 15 s"), d.bl_state == BL_OFF && d.led == 0 &&
          d.led_sets == 2);
    check(L(tag, "off and silent: the core idled the same pass"),
          d.cpu_idled && d.boost == 0 && d.unboosts == 1);
    for (t = base + S(31); t != base + S(600); t += S(1)) pass(&d, t);
    check(L(tag, "off: nothing more, no second unboost"),
          d.led_sets == 2 && d.unboosts == 1 && d.boost == 0 && !d.bad);

    /* 4. Wake from OFF on input: lit at full, the core back up that pass. */
    t = base + S(600);
    input(&d, t);
    pass(&d, t);
    check(L(tag, "wake from off: lit at 32"), d.bl_state == BL_FULL && d.led == 32);
    check(L(tag, "wake from off: re-boosted the same pass"),
          !d.cpu_idled && d.boost == 1 && d.boosts == 1);
    /* 3. The next dim is a whole timeout after the wake. */
    pass(&d, t + S(15));
    check(L(tag, "after the wake: lit through a whole timeout"), d.bl_state == BL_FULL);
    pass(&d, t + S(15) + 1u);
    check(L(tag, "after the wake: dims again"), d.bl_state == BL_DIM && d.led == 8);

    /* 4. Wake from DIM on input: the core was never idled. */
    input(&d, t + S(20));
    pass(&d, t + S(20));
    check(L(tag, "wake from dim: lit at 32, no CPU step"),
          d.bl_state == BL_FULL && d.led == 32 && d.boosts == 1 && d.unboosts == 1);
    /* An idle stamp that has gone back to 0 does not relight a dimmed
     * screen: the input site does. Model a pass with no input site. */
    pass(&d, t + S(20) + S(15) + 1u);
    check(L(tag, "dimmed again"), d.bl_state == BL_DIM);
    d.last_input = t + S(40);              /* a restamp with no relight */
    pass(&d, t + S(40));
    check(L(tag, "a restamp alone leaves it dimmed"), d.bl_state == BL_DIM && d.led == 8);

    /* 5. Playing: the screen times out as usual; the core stays up. */
    boot(&d, base, 15, 32);
    d.playing = 1;
    pass(&d, base + S(15) + 1u);
    pass(&d, base + S(30) + 1u);
    check(L(tag, "playing: dims and goes out on the same clock"),
          d.bl_state == BL_OFF && d.led == 0 && d.led_sets == 2);
    check(L(tag, "playing, dark: the core is NOT idled"), !d.cpu_idled && d.boost == 1);
    d.playing = 0;                         /* paused in the dark             */
    pass(&d, base + S(40));
    check(L(tag, "paused in the dark: idled"), d.cpu_idled && d.boost == 0);
    d.playing = 1;                         /* playback starts, still dark    */
    pass(&d, base + S(41));
    check(L(tag, "playing again, dark: re-boosted, screen still off"),
          !d.cpu_idled && d.boost == 1 && d.bl_state == BL_OFF);
    check(L(tag, "playing: balanced"), d.boosts == d.unboosts && !d.bad);

    /* 2. Never: hours of idle, lit, core up. */
    boot(&d, base, 0, 32);
    for (t = base; t != base + S(7 * 514); t += S(7)) pass(&d, t);
    check(L(tag, "never: lit after an hour, core up"),
          d.bl_state == BL_FULL && d.led_sets == 0 && !d.cpu_idled && d.boost == 1);
    /* Never chosen while dimmed: stays dimmed until an input. */
    boot(&d, base, 15, 32);
    pass(&d, base + S(16));
    d.secs = 0;
    for (t = base + S(17); t != base + S(1800); t += S(1)) pass(&d, t);
    check(L(tag, "never chosen while dimmed: stays dimmed"),
          d.bl_state == BL_DIM && d.led == 8 && d.led_sets == 1);

    /* Sparse passes (a long blocking read): one step each. */
    boot(&d, base, 5, 20);
    pass(&d, base + S(300));
    check(L(tag, "sparse: one pass far past both -> dim only, at 5"),
          d.bl_state == BL_DIM && d.led == 5);
    pass(&d, base + S(300) + 1u);
    check(L(tag, "sparse: the next pass -> off"), d.bl_state == BL_OFF && d.cpu_idled);

    /* Suspend from the dark, idled state (the sleep timer): run_ui clears
     * cpu_idled after the wake's own boost, and the loop stays balanced. */
    boot(&d, base, 15, 32);
    pass(&d, base + S(16));
    pass(&d, base + S(31));
    check(L(tag, "suspend: idled before"), d.cpu_idled && d.boost == 0);
    suspend(&d, base + S(100), 1);
    pass(&d, base + S(100));
    check(L(tag, "suspend, cpu_idled cleared: no second boost, refcount 1"),
          d.boost == 1 && d.boosts == 0 && !d.bad);
    /* The same without the clear: what run_ui's comment guards against. */
    boot(&d, base, 15, 32);
    pass(&d, base + S(16));
    pass(&d, base + S(31));
    suspend(&d, base + S(100), 0);
    pass(&d, base + S(100));
    check(L(tag, "suspend, cpu_idled left set: the policy boosts to 2"),
          d.boost == 2 && d.bad);
}

/* 6. The balance under a random walk of everything run_ui can do. */
static void test_balance(uint32_t base)
{
    static const int secs[] = { 0, 5, 10, 15, 30, 60 };
    ui_t d;
    unsigned seed = 12345u;
    int bad = 0, steps = 0, idles = 0;
    for (int run = 0; run < 200; run++) {
        boot(&d, base, 5, 32);
        uint32_t t = base;
        for (int i = 0; i < 2000; i++) {
            seed = seed * 1103515245u + 12345u;
            unsigned r = (seed >> 8) & 0xFFFFu;
            /* Mostly small steps, sometimes a long dark spell. */
            t += (r & 7u) == 0 ? S(1 + (r >> 4) % 90) : 10000u * (1u + (r >> 4) % 50u);
            switch ((r >> 3) % 23u) {
            case 0: case 1: input(&d, t); break;
            case 2: d.playing = !d.playing; break;
            case 3: d.secs = secs[(r >> 5) % 6u]; break;
            case 4: d.bright = 1 + (int)((r >> 6) % 32u); break;
            case 5: suspend(&d, t, 1); break;
            default: break;
            }
            pass(&d, t);
            steps++;
            if (d.cpu_idled) idles++;
            if (d.boost != (d.cpu_idled ? 0 : 1)) bad = 1;
            if (d.cpu_idled && (d.bl_state != BL_OFF || d.playing)) bad = 1;
            if (!d.cpu_idled && d.bl_state == BL_OFF && !d.playing) bad = 1;
        }
        if (d.bad) bad = 1;
        /* Each unboost has its restore: by the loop's boost, or by a
         * suspend's (which run_ui then books by clearing cpu_idled). */
        if (d.unboosts != d.boosts + d.susp_restores + d.cpu_idled) bad = 1;
    }
    check("balance: refcount == !cpu_idled after every pass, every unboost "
          "restored exactly once, 400k passes", !bad);
    check("balance: the walk reached the idled state", idles > steps / 50);
}

int main(void)
{
    test_steps();
    test_timeline(0x40000000u, "mid:");
    /* The stamps straddle the 32-bit wrap: boot a few seconds before it, the
     * dim and off deadlines after. */
    test_timeline(0xFFFFFFFFu - S(4), "wrap:");
    test_timeline(0xFFFFFFFFu - S(20), "wrap20:");
    test_timeline(0xFFFFFFFFu - S(200), "wrap200:");
    test_balance(0xFFFFFFFFu - S(30));
    if (g_fail) {
        printf("backlightpolicy: FAIL\n");
        return 1;
    }
    printf("backlightpolicy: all pass\n");
    return 0;
}
