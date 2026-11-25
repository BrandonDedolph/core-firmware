/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/drivepower_test.c — the drive-power policy on the host.
 *
 * drivepower.c is pure (the caller passes the clock and the drive's state),
 * so every case is a main-loop pass at an exact microsecond stamp. Pinned:
 *   1. Idle park: nothing playing, the drive up, idle > DISK_IDLE_US.
 *   2. No idle park within UI_DISK_GRACE_US of a UI read.
 *   3. Lock park at once (with the SOFT commits first) unless the player
 *      still needs the drive; never while playing or already parked.
 *   4. A refused STANDBY is retried no sooner than DISK_STANDBY_RETRY_US.
 *   5. The hold: idle <= DISK_BROWSE_HOLD_US or a UI read in its grace, and
 *      never while locked.
 *   6. The player's burst park: the hold, the HAL flag after a menu read, a
 *      refused STANDBY (park_ok), the buffer's low mark.
 *   7. The paused-stocking test behind player_disk_busy.
 *   8. Quick-park accounting, by kind, only on edges.
 *   9. 1-8 again across the 32-bit microsecond wrap.
 */

#include <stdio.h>
#include <string.h>

#include "drivepower.h"

static int g_fail;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) g_fail = 1;
    return cond;
}

#define S(n)   ((uint32_t)(n) * 1000000u)      /* seconds -> us */

/* A fresh state whose last UI read is long past (well outside the grace
 * for the whole of a scenario: scenarios span minutes, the wrap is 71.6). */
static void fresh(drivepower_t *dp, uint32_t base)
{
    memset(dp, 0, sizeof *dp);
    drivepower_note_ui_read(dp, base - S(600));
}

static drivepower_out_t feed(const drivepower_t *dp, uint32_t now, uint32_t idle,
                             int locked, int playing, int parked, int busy)
{
    drivepower_in_t in;
    in.now_us      = now;
    in.idle_us     = idle;
    in.locked      = locked;
    in.playing     = playing;
    in.parked      = parked;
    in.player_busy = busy;
    return drivepower_idle_feed(dp, &in);
}

static char g_lbl[128];
static const char *L(const char *tag, const char *what)
{
    snprintf(g_lbl, sizeof g_lbl, "%s %s", tag, what);
    return g_lbl;
}

static void test_ui_half(uint32_t base, const char *tag)
{
    drivepower_t dp;
    drivepower_out_t o;

    /* 1. Idle park. */
    fresh(&dp, base);
    o = feed(&dp, base, DISK_IDLE_US, 0, 0, 0, 0);
    check(L(tag, "idle == DISK_IDLE_US: no park yet"), !o.standby);
    o = feed(&dp, base, DISK_IDLE_US + 1u, 0, 0, 0, 0);
    check(L(tag, "idle past DISK_IDLE_US: park"), o.standby && !o.flush_first);
    o = feed(&dp, base, DISK_IDLE_US + 1u, 0, 1, 0, 0);
    check(L(tag, "idle park: never while playing"), !o.standby);
    o = feed(&dp, base, DISK_IDLE_US + 1u, 0, 0, 1, 0);
    check(L(tag, "idle park: never on a parked drive"), !o.standby);
    o = feed(&dp, base, S(3600), 0, 0, 0, 1);
    check(L(tag, "idle park: player_busy is the lock's test only"), o.standby);

    /* 2. The UI read grace. */
    fresh(&dp, base);
    uint32_t t_read = base + S(20);
    drivepower_note_ui_read(&dp, t_read);
    o = feed(&dp, t_read, S(60), 0, 0, 0, 0);
    check(L(tag, "grace: no idle park at the read"), !o.standby && o.hold);
    o = feed(&dp, t_read + UI_DISK_GRACE_US - 1u, S(60), 0, 0, 0, 0);
    check(L(tag, "grace: no idle park 1 us before it ends"), !o.standby && o.hold);
    o = feed(&dp, t_read + UI_DISK_GRACE_US, S(60), 0, 0, 0, 0);
    check(L(tag, "grace: idle park once it ends"), o.standby && !o.hold);
    o = feed(&dp, t_read + UI_DISK_GRACE_US - 1u, S(60), 1, 0, 0, 0);
    check(L(tag, "grace: the lock park does not wait for it"),
          o.standby && o.flush_first && !o.hold);

    /* 3. Lock park. */
    fresh(&dp, base);
    o = feed(&dp, base, 0, 1, 0, 0, 0);
    check(L(tag, "lock: park at once, commits first"), o.standby && o.flush_first);
    o = feed(&dp, base, 0, 1, 0, 0, 1);
    check(L(tag, "lock: not while the player needs the drive"), !o.standby);
    o = feed(&dp, base, S(3600), 1, 0, 0, 1);
    check(L(tag, "lock + busy + idle: the idle park still fires, no commits"),
          o.standby && !o.flush_first);
    o = feed(&dp, base, 0, 1, 1, 0, 0);
    check(L(tag, "lock: not while playing"), !o.standby);
    o = feed(&dp, base, 0, 1, 0, 1, 0);
    check(L(tag, "lock: not on a parked drive"), !o.standby);

    /* 4. A refused STANDBY. */
    fresh(&dp, base);
    uint32_t t_fail = base + S(30);
    drivepower_standby_result(&dp, -1, t_fail);
    o = feed(&dp, t_fail, S(60), 0, 0, 0, 0);
    check(L(tag, "refused: not re-issued on the same pass"), !o.standby);
    o = feed(&dp, t_fail + DISK_STANDBY_RETRY_US - 1u, S(60), 0, 0, 0, 0);
    check(L(tag, "refused: not 1 us before the retry"), !o.standby);
    o = feed(&dp, t_fail + DISK_STANDBY_RETRY_US - 1u, 0, 1, 0, 0, 0);
    check(L(tag, "refused: the lock park waits for the retry too"), !o.standby);
    o = feed(&dp, t_fail + DISK_STANDBY_RETRY_US, S(60), 0, 0, 0, 0);
    check(L(tag, "refused: retried at DISK_STANDBY_RETRY_US"), o.standby);
    drivepower_standby_result(&dp, 0, t_fail + DISK_STANDBY_RETRY_US);
    o = feed(&dp, t_fail + DISK_STANDBY_RETRY_US + 1u, S(60), 0, 0, 0, 0);
    check(L(tag, "accepted: no retry gate"), o.standby);

    /* 5. The hold. */
    fresh(&dp, base);
    o = feed(&dp, base, 0, 0, 1, 0, 0);
    check(L(tag, "hold: just pressed"), o.hold);
    o = feed(&dp, base, DISK_BROWSE_HOLD_US, 0, 1, 0, 0);
    check(L(tag, "hold: idle == DISK_BROWSE_HOLD_US"), o.hold);
    o = feed(&dp, base, DISK_BROWSE_HOLD_US + 1u, 0, 1, 0, 0);
    check(L(tag, "hold: released past it"), !o.hold);
    o = feed(&dp, base, 0, 1, 1, 0, 0);
    check(L(tag, "hold: never while locked"), !o.hold);
    drivepower_note_ui_read(&dp, base);
    o = feed(&dp, base + 1u, S(60), 0, 1, 0, 0);
    check(L(tag, "hold: a UI read keeps it"), o.hold);
    o = feed(&dp, base + 1u, S(60), 1, 1, 0, 0);
    check(L(tag, "hold: a UI read does not keep it while locked"), !o.hold);
    o = feed(&dp, base + UI_DISK_GRACE_US, S(60), 0, 1, 0, 0);
    check(L(tag, "hold: released once the grace ends"), !o.hold);

    /* 8. Quick-park accounting. */
    memset(&dp, 0, sizeof dp);
    check(L(tag, "park: no edge while spinning"), !drivepower_park_edge(&dp, 0));
    check(L(tag, "park: an edge on the first park"), drivepower_park_edge(&dp, 1));
    uint32_t t = base + S(5);
    drivepower_park_note(&dp, 1, 0, t);
    check(L(tag, "park: counted as not playing"),
          drivepower_parks(&dp, 0) == 1 && drivepower_parks(&dp, 1) == 0);
    check(L(tag, "park: no edge while parked"), !drivepower_park_edge(&dp, 1));
    drivepower_park_note(&dp, 1, 1, t + 1u);       /* not an edge: a no-op */
    check(L(tag, "park: a non-edge note changes nothing"),
          drivepower_parks(&dp, 0) == 1 && drivepower_parks(&dp, 1) == 0);
    check(L(tag, "park: an edge on the wake"), drivepower_park_edge(&dp, 0));
    /* Woken while playing: counted against the park's kind, not the wake's. */
    drivepower_park_note(&dp, 0, 1, t + DISK_PARK_QUICK_US - 1u);
    check(L(tag, "park: woken 1 us inside DISK_PARK_QUICK_US is quick"),
          drivepower_quick_parks(&dp, 0) == 1 && drivepower_quick_parks(&dp, 1) == 0);

    t = base + S(100);
    drivepower_park_note(&dp, 1, 1, t);
    drivepower_park_note(&dp, 0, 0, t + DISK_PARK_QUICK_US);
    check(L(tag, "park: a playing park, woken at DISK_PARK_QUICK_US, is not quick"),
          drivepower_parks(&dp, 1) == 1 && drivepower_quick_parks(&dp, 1) == 0);
    drivepower_park_note(&dp, 1, 1, t + S(200));
    drivepower_park_note(&dp, 0, 1, t + S(201));
    check(L(tag, "park: a playing park woken at once is quick"),
          drivepower_parks(&dp, 1) == 2 && drivepower_quick_parks(&dp, 1) == 1);
    check(L(tag, "park: the not-playing counts are untouched"),
          drivepower_parks(&dp, 0) == 1 && drivepower_quick_parks(&dp, 0) == 1);
}

static void test_player_half(void)
{
    drivepower_burst_t b;
    const uint32_t low = 1024u * 1024u;
    memset(&b, 0, sizeof b);

    /* 6. The burst park. */
    check("burst: topped up, spinning -> park",
          drivepower_burst_may_park(&b, 0, 0, low + 1u, low));
    check("burst: not at the low mark",
          !drivepower_burst_may_park(&b, 0, 0, low, low));
    drivepower_burst_hold(&b, 7);
    check("burst: hold normalises to 1", b.hold == 1);
    check("burst: held -> no park",
          !drivepower_burst_may_park(&b, 0, 0, low + 1u, low));
    drivepower_burst_hold(&b, 0);
    check("burst: released -> park",
          drivepower_burst_may_park(&b, 0, 0, low + 1u, low));

    drivepower_burst_parked(&b, 0);                /* STANDBY accepted */
    check("burst: own park, HAL parked -> one STANDBY per park",
          !drivepower_burst_may_park(&b, 1, 1, low + 1u, low));
    check("burst: own park, a menu read spun it up -> park again",
          drivepower_burst_may_park(&b, 1, 0, low + 1u, low));
    drivepower_burst_hold(&b, 1);
    check("burst: ...but not while held",
          !drivepower_burst_may_park(&b, 1, 0, low + 1u, low));
    drivepower_burst_hold(&b, 0);

    drivepower_burst_parked(&b, -1);               /* STANDBY refused */
    check("burst: refused -> park_ok clear", b.park_ok == 0);
    check("burst: refused -> not retried every pass",
          !drivepower_burst_may_park(&b, 1, 0, low + 1u, low));
    check("burst: refused -> retried after the next burst clears own_parked",
          drivepower_burst_may_park(&b, 0, 0, low + 1u, low));

    /* 7. Paused stocking. */
    check("stock: paused, short -> busy",
          drivepower_paused_stocking(1, 0, 0, low - 1u, low));
    check("stock: paused, at the mark -> not busy",
          !drivepower_paused_stocking(1, 0, 0, low, low));
    check("stock: playing -> not this test",
          !drivepower_paused_stocking(0, 0, 0, 0, low));
    check("stock: end of file -> not busy",
          !drivepower_paused_stocking(1, 1, 0, 0, low));
    /* diskbuf sets err = 1 on a failed read (codecs/diskbuf.c); test both signs. */
    check("stock: read error -> not busy",
          !drivepower_paused_stocking(1, 0, 1, 0, low));
    check("stock: negative error -> not busy",
          !drivepower_paused_stocking(1, 0, -5, 0, low));
}

/* Boot: a zero state, as the static in main.c is. The last-UI-read stamp of
 * 0 reads as a read at boot, so the first UI_DISK_GRACE_US hold the drive. */
static void test_boot(void)
{
    drivepower_t dp;
    drivepower_out_t o;
    memset(&dp, 0, sizeof dp);
    o = feed(&dp, S(1), S(60), 0, 0, 0, 0);
    check("boot: within the grace of stamp 0 -> no idle park, hold", !o.standby && o.hold);
    o = feed(&dp, UI_DISK_GRACE_US, S(60), 0, 0, 0, 0);
    check("boot: past it -> idle park", o.standby && !o.hold);
    check("boot: no park edge on a spinning drive", !drivepower_park_edge(&dp, 0));
}

int main(void)
{
    test_boot();
    test_ui_half(0x40000000u, "mid:");
    /* Every scenario's stamps straddle the wrap: the first lands a few
     * seconds before it, the deadlines after. */
    test_ui_half(0xFFFFFFFFu - S(4), "wrap:");
    test_ui_half(0xFFFFFFFFu - S(25), "wrap25:");
    test_ui_half(0xFFFFFFFFu - S(120), "wrap120:");
    test_player_half();
    if (g_fail) {
        printf("drivepower: FAIL\n");
        return 1;
    }
    printf("drivepower: all pass\n");
    return 0;
}
