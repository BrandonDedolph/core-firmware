/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/drivepower.c — see drivepower.h.
 */

#include "drivepower.h"

void drivepower_note_ui_read(drivepower_t *dp, uint32_t now_us)
{
    dp->ui_disk_us = now_us;
}

drivepower_out_t drivepower_idle_feed(const drivepower_t *dp,
                                      const drivepower_in_t *in)
{
    drivepower_out_t out;
    int ui_reading = (uint32_t)(in->now_us - dp->ui_disk_us) < UI_DISK_GRACE_US;
    int lock_park  = in->locked && !in->player_busy;
    int may_standby = !dp->standby_failed ||
                      (uint32_t)(in->now_us - dp->standby_fail_us) >= DISK_STANDBY_RETRY_US;

    out.standby = !in->playing && !in->parked && may_standby &&
                  ((in->idle_us > DISK_IDLE_US && !ui_reading) || lock_park);
    out.flush_first = out.standby && lock_park;
    out.hold = !in->locked && (in->idle_us <= DISK_BROWSE_HOLD_US || ui_reading);
    return out;
}

void drivepower_standby_result(drivepower_t *dp, int rc, uint32_t now_us)
{
    dp->standby_failed  = (rc != 0);
    dp->standby_fail_us = now_us;
}

int drivepower_park_edge(const drivepower_t *dp, int parked)
{
    return parked != dp->park_was;
}

void drivepower_park_note(drivepower_t *dp, int parked, int playing,
                          uint32_t now_us)
{
    if (parked == dp->park_was) {
        return;
    }
    if (parked) {
        dp->park_kind  = playing ? 1 : 0;
        dp->park_at_us = now_us;
        dp->park_n[dp->park_kind]++;
    } else if ((uint32_t)(now_us - dp->park_at_us) < DISK_PARK_QUICK_US) {
        dp->park_quick[dp->park_kind]++;
    }
    dp->park_was = parked;
}

uint32_t drivepower_parks(const drivepower_t *dp, int playing)
{
    return dp->park_n[playing ? 1 : 0];
}

uint32_t drivepower_quick_parks(const drivepower_t *dp, int playing)
{
    return dp->park_quick[playing ? 1 : 0];
}

void drivepower_burst_hold(drivepower_burst_t *b, int hold)
{
    b->hold = hold ? 1 : 0;
}

int drivepower_burst_may_park(const drivepower_burst_t *b, int own_parked,
                              int hal_parked, uint32_t ahead, uint32_t low)
{
    return !b->hold &&
           (!own_parked || (b->park_ok && !hal_parked)) &&
           ahead > low;
}

void drivepower_burst_parked(drivepower_burst_t *b, int rc)
{
    b->park_ok = (rc == 0);
}

int drivepower_paused_stocking(int paused, int eos, int err, uint32_t ahead,
                               uint32_t low)
{
    return paused && !eos && err == 0 && ahead < low;
}
