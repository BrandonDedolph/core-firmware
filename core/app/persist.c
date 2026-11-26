/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/persist.c — see persist.h.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/ata.h"
#include "hw/battery.h"
#include "../player/player.h"
#include "../kernel/config.h"
#include "../kernel/cfg_commit.h"
#include "../kernel/evlog.h"
#include "../kernel/otg_store.h"
#include "state.h"
#include "util.h"
#include "otg.h"
#include "persist.h"

/* ---------------------------------------------------------------------------
 * Settings persistence (kernel/config.c) — DEBOUNCED.
 *
 * A disk write per wheel tick would mean dozens of writes for one volume sweep:
 * pointless drive wear, a spin-up the user can hear, and dozens of chances to
 * be interrupted mid-write. So every mutation of g_settings just marks the
 * state dirty and stamps the time; the main loop commits ONE write once the
 * user has stopped fiddling, and leaving the Settings screen forces it
 * immediately (the natural "I'm done" moment).
 *
 * settings_touch() is called from every site that changes g_settings. If you
 * add another, call it there too — a missed call means the change is simply
 * not persisted, which is the safe direction to fail.
 * ------------------------------------------------------------------------- */
/* The pending-change state and the gate itself are kernel/cfg_commit.c —
 * pure, and tested on the host — so this file only gathers what the gate
 * needs to know and carries out its verdict. */
cfg_commit_t g_cfg_commit;

void settings_touch(void)
{
    cfg_commit_touch(&g_cfg_commit, mmio_read32(USEC_TIMER_ADDR));
}

/*
 * Commit a pending save. `mode` is CFG_COMMIT_IDLE (the main loop: debounced,
 * deferred while the drive is parked under a live player), CFG_COMMIT_FORCE
 * (suspend, power-off: now) or CFG_COMMIT_LAST (the DISKSAFE flush, exempt
 * from the battery gate). config_save() and evlog_flush() (evlog_commit,
 * below) are the only two things in the firmware that write to the user's
 * disk, and both go through this gate, so its verdict is the whole write
 * policy — see cfg_commit.h.
 *
 * Two things the gate is there to get right, because the code that stood
 * here got both wrong: a write into a PARKED drive is preceded by
 * ata_wakeup() (the forced commits at suspend/power-off went straight in,
 * and the spin-up then ran inside ata_write_raw's DRQ budget — on overrun
 * ata_recover soft-reset the channel mid-spin-up and the save returned -2);
 * and a write that fails leaves the change PENDING, for a retry one debounce
 * later, bounded (the pending flag was cleared before the write, so that -2
 * silently discarded the change — the resume position captured a line
 * earlier included). The previous good record is intact either way.
 */
void settings_commit(int mode)
{
    cfg_commit_env_t env;
    env.now_us        = mmio_read32(USEC_TIMER_ADDR);
    env.parked        = ata_is_parked();
    env.player_active = player_active();
    env.battery_ok    = battery_disk_writes_allowed();
    env.writable      = config_writable();
    int gate = cfg_commit_gate(&g_cfg_commit, mode, &env);
    if (gate == CFG_GATE_DEFER_LOG) {
        /* Once per refusal episode, never once per pass: a UART line is
         * milliseconds of blocking TX, and this runs thousands of times a
         * second while playing. */
        uart_puts("core: cfg save deferred — battery below disk-safe\n");
        return;
    }
    if (gate != CFG_GATE_WRITE && gate != CFG_GATE_WRITE_WAKE) {
        return;
    }
    if (gate == CFG_GATE_WRITE_WAKE) {
        /* Pre-pay the spin-up on the read path, which is built to wait out
         * the multi-second wake, rather than inside the write's DRQ budget.
         * A failed wake is not fatal here: the write below reports its own
         * result, and a failure keeps the change pending. */
        (void)ata_wakeup();
    }
    int rc      = config_save(&g_settings);
    int pending = cfg_commit_result(&g_cfg_commit, rc,
                                    mmio_read32(USEC_TIMER_ADDR));
    uart_puts("core: cfg save rc ");
    uart_put_hex32((uint32_t)rc);
    uart_puts(" seq ");
    uart_put_hex32(config_seq());
    if (pending) {
        uart_puts(" (retry pending)");
    }
    uart_putc('\n');
}

/*
 * Flush the event log through the same gate, with the same inputs. `mode`
 * as settings_commit's. One block at most; the outcome is narrated — which
 * puts the narration into the NEXT block, cheaply — except the quiet
 * repeat of a battery refusal, for the same reason settings_commit stays
 * quiet: this runs every main-loop pass.
 */
void evlog_commit(int mode)
{
    cfg_commit_env_t env;
    env.now_us        = mmio_read32(USEC_TIMER_ADDR);
    env.parked        = ata_is_parked();
    env.player_active = player_active();
    env.battery_ok    = battery_disk_writes_allowed();
    env.writable      = 0;                /* evlog knows its own */
    int r = evlog_flush(mode, &env);
    switch (r) {
    case EVLOG_FLUSH_WROTE:
        uart_puts("core: evlog blk ");
        uart_put_hex32(evlog_seq() - 1u);
        uart_puts(mode == CFG_COMMIT_IDLE ? "\n" : " final\n");
        break;
    case EVLOG_FLUSH_DEFERRED:
        uart_puts("core: evlog flush deferred — battery below disk-safe\n");
        break;
    case EVLOG_FLUSH_FAILED:
        uart_puts("core: evlog flush rc ");
        uart_put_hex32((uint32_t)evlog_last_rc());
        uart_puts(" fails ");
        uart_dec((int)evlog_failures());
        if (!evlog_enabled()) {
            uart_puts(" — log OFF for this session");
        }
        uart_putc('\n');
        break;
    default:
        break;
    }
}

/*
 * The On-The-Go live list's commit, through the SAME gate with the same
 * inputs — its cfg_commit_t lives inside kernel/otg_store.c, as the event
 * log's lives inside evlog.c, so this is the whole of app/persist.c's share.
 * `mode` as settings_commit's.
 */
void otg_commit(int mode)
{
    cfg_commit_env_t env;
    env.now_us        = mmio_read32(USEC_TIMER_ADDR);
    env.parked        = ata_is_parked();
    env.player_active = player_active();
    env.battery_ok    = battery_disk_writes_allowed();
    env.writable      = 0;                /* otg_store knows its own */
    switch (otg_store_commit(mode, &env, &g_otg)) {
    case OTG_COMMIT_WROTE:
        uart_puts("core: otg save rc 00000000 seq ");
        uart_put_hex32(otg_store_seq());
        uart_putc('\n');
        break;
    case OTG_COMMIT_DEFERRED:
        uart_puts("core: otg save deferred — battery below disk-safe\n");
        break;
    case OTG_COMMIT_FAILED:
        uart_puts("core: otg save rc ");
        uart_put_hex32((uint32_t)otg_store_last_rc());
        uart_puts(otg_store_pending() ? " (retry pending)\n" : " (given up)\n");
        break;
    default:
        break;
    }
}
