/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/headless/headless.h — the control surface of the headless firmware
 * harness (docs/testing-headless.md).
 *
 * WHAT THIS IS. The REAL kernel/main.c, player, file system, codecs and UI,
 * compiled for the host with -DMMIO_MOCK and linked against this backend
 * instead of hal/hw/. main.c never talks to hal.h alone — it calls the hw
 * driver APIs (hw/lcd.h, hw/ata.h, hw/clickwheel.h, ...) and reads USEC_TIMER
 * through the hw/mmio.h seam — so the backend is cut at THAT level:
 *
 *   faked here (headless_hw.c)       real, linked unmodified
 *   ------------------------------   ---------------------------------------
 *   LCD/BCM (a panel buffer)         kernel/main.c, console.c, config.c,
 *   ATA (a disk-image file, r/w)       evlog.c, otg_store.c, idlesleep.c ...
 *   click wheel (a scripted queue)   player/player.c + pcm_ring + codecs
 *   audio DMA/codec (a frame count)  fs/fat32.c, fs/m3u.c, library/, ui/
 *   backlight, piezo, power/PMU      hal/hw/battery.c (gauge + low-batt policy)
 *   headphone jack, UART             hal/hw/rtc.c (clock over the fake I2C)
 *   I2C: a fake PCF50605 (ADC, RTC)
 *   USEC_TIMER / CPU_CTL (fake time)
 *   kernel clock/timer/sched/panic stand-ins
 *
 * TIME. Nothing reads the host clock. Fake time is a 64-bit microsecond count
 * that moves only when the firmware waits: a CPU_CTL countdown (cpu_wait_ms /
 * cpu_wait_us), sleep_ms(), a drive spin-up, an audio drain — and by 1 us per
 * USEC_TIMER read, so a firmware spin on the counter terminates. A run is
 * therefore bit-for-bit reproducible.
 *
 * THREADS, DETERMINISTICALLY. kernel_main() never returns, so it runs on its
 * own thread and the test drives it through a baton: exactly one of the two
 * threads runs at any moment. hl_run_ms() hands the baton to the firmware
 * until fake time has advanced by that much, then the firmware hands it back
 * from inside whatever wait it was in. Between runs the firmware is frozen,
 * so everything below can be read without locks.
 *
 * "ISRs". The 100 Hz tick (wheel sampling) and the audio DMA (pulling PCM from
 * the player's source callback at the stream's rate) are serviced as fake time
 * advances, from whichever firmware call advanced it — which is what an IRQ on
 * the device does.
 */
#ifndef CORE_HAL_HEADLESS_H
#define CORE_HAL_HEADLESS_H

#include <stddef.h>
#include <stdint.h>

#define HL_W 320
#define HL_H 240

/* WHEEL_BTN_* values (hw/clickwheel.h), repeated so a test need not include
 * the hw headers. headless_hw.c static-asserts they agree. */
enum {
    HL_BTN_SELECT = 1u << 0,
    HL_BTN_MENU   = 1u << 1,
    HL_BTN_PLAY   = 1u << 2,
    HL_BTN_LEFT   = 1u << 3,
    HL_BTN_RIGHT  = 1u << 4,
};

/* Why the firmware thread stopped for good (hl_parked()). */
typedef enum {
    HL_RUNNING = 0,
    HL_PARK_POWEROFF,       /* power_standby() accepted (hl_cfg.standby_accept) */
    HL_PARK_REBOOT,         /* power_reboot()                                    */
    HL_PARK_DISKMODE,       /* power_enter_disk_mode()                           */
    HL_PARK_IDLE_FALLBACK,  /* sched_start(): no LCD / mount failed              */
    HL_PARK_PANIC,          /* panic()                                           */
} hl_park_t;

/* Timeline of the hardware-visible actions, for ORDER assertions ("on wake the
 * first full present came before the backlight"). */
typedef enum {
    HL_EV_PRESENT_FULL = 1,
    HL_EV_PRESENT_RECT,
    HL_EV_PRESENT_REFUSED,  /* a present while the panel was slept            */
    HL_EV_BACKLIGHT,        /* arg = level                                     */
    HL_EV_LCD_SLEEP,
    HL_EV_LCD_WAKE,
    HL_EV_ATA_STANDBY,
    HL_EV_ATA_SLEEP,
    HL_EV_ATA_WAKEUP,
    HL_EV_ATA_SPINUP,       /* a read/write/wakeup that had to spin the drive  */
    HL_EV_ATA_WRITE,        /* arg = lba                                       */
    HL_EV_AUDIO_INIT,       /* arg = rate                                      */
    HL_EV_AUDIO_START,
    HL_EV_AUDIO_STOP,
    HL_EV_AUDIO_SUSPEND,    /* codec powered down across a pause               */
    HL_EV_AUDIO_WAKE,
    HL_EV_AUDIO_CLOSE,
    HL_EV_CPU_BOOST,        /* arg = refcount after                            */
    HL_EV_CPU_UNBOOST,
    HL_EV_PMU_STANDBY,      /* power_standby() called (accepted or refused)    */
} hl_ev_kind_t;

typedef struct {
    uint64_t t_us;
    int      kind;          /* hl_ev_kind_t */
    uint32_t arg;
} hl_ev_t;

/* Knobs. Set before hl_boot(); the defaults are a device on battery with the
 * headphones in. */
typedef struct {
    int      battery_mv;        /* PMU ADC reading (default 3950)              */
    int      external_power;    /* 1 = USB on the dock connector               */
    int      headphones;        /* 1 seated, 0 out (hal_headphones_present)    */
    int      hold;              /* the Hold switch at boot                     */
    uint32_t spinup_ms;         /* fake time a parked drive takes (default 800) */
    int      standby_accept;    /* 1: power_standby() powers off (parks)        */
    uint32_t rtc_epoch;         /* RTC at boot, UTC (default 2026-11-01 12:00) */
    int      verbose;           /* echo the UART to stdout (env HL_VERBOSE=1)  */
} hl_cfg_t;

extern hl_cfg_t hl_cfg;

/* Writes the firmware may make to the image: [lba, lba+count) ranges. Writes
 * outside every range are refused (-1) and counted in hl_stats.bad_writes.
 * Empty = no guard. Loaded from the image builder's .ranges file by the test. */
int  hl_allow_writes(uint32_t lba, uint32_t count);

/* Open `disk` (read-write) as the drive and start the firmware thread; it
 * runs until `ms` of fake time have passed. 0 on success. */
int  hl_boot(const char *disk, uint32_t ms);

/* Let the firmware run for `ms` of fake time. Returns at once if parked. */
void hl_run_ms(uint32_t ms);

/* Run in `step_ms` slices until pred() is true or `max_ms` elapse. Returns 1
 * when the predicate held. */
int  hl_run_until(int (*pred)(void *), void *arg, uint32_t step_ms,
                  uint32_t max_ms);

/* Input, scheduled from the current fake time. Nothing happens until the
 * firmware runs. A "row" of wheel motion is 4 raw steps (WHEEL_CLICKS_PER_ITEM),
 * delivered one row per 150 ms so the acceleration curve stays at rest. */
void hl_press(unsigned btn, uint32_t hold_ms);       /* down now, up later */
void hl_scroll(int rows);                            /* + = clockwise      */
void hl_set_hold(int on);
void hl_set_headphones(int present);
void hl_set_external_power(int on);
void hl_set_battery_mv(int mv);

/* Convenience: press for 60 ms then run `settle_ms`. */
void hl_tap(unsigned btn, uint32_t settle_ms);

/* What is on the LCD (the last presented frame, RGB565, row-major). */
const uint16_t *hl_panel(void);

/* Counters and states, readable while the firmware is frozen. */
typedef struct {
    uint64_t now_us;
    uint32_t presents_full, presents_rect, presents_refused;
    int      lcd_slept;
    int      backlight;           /* last backlight_set() level              */
    uint32_t ata_reads, ata_writes, ata_spinups;
    uint32_t bad_writes;          /* outside hl_allow_writes / misaligned    */
    int      ata_parked, ata_slept;
    int      audio_open, audio_running, audio_cold;
    uint32_t audio_rate;
    uint64_t audio_frames_played; /* DAC frames clocked since boot           */
    uint64_t audio_frames_pulled; /* frames the source actually delivered    */
    uint64_t audio_nonzero;       /* delivered samples that were not silence */
    uint32_t audio_underruns;     /* short DMA refills THIS stream (hw: reset
                                   * by hal_audio_init; a queue ending is one) */
    uint32_t audio_underruns_total;
    int      cpu_boost;           /* refcount                                */
    uint32_t piezo_clicks;
    uint32_t pmu_standby_calls;
    int      volume;
} hl_stats_t;

hl_stats_t hl_stats(void);
hl_park_t  hl_parked(void);

/* Everything the firmware printed on the UART, NUL-terminated. */
const char *hl_log(void);
size_t      hl_log_len(void);

/* The timeline: hl_events() is every event so far, in order (*n of them);
 * hl_events_mark() is the index the next one will get, for "since here". */
size_t         hl_events_mark(void);
const hl_ev_t *hl_events(size_t *n);

/* Dump the panel as a 24-bit BMP. 0 on success. */
int hl_write_bmp(const char *path);

#endif /* CORE_HAL_HEADLESS_H */
