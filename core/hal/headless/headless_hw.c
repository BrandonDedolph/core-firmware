/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/headless/headless_hw.c — the headless backend: host fakes of the
 * hw driver APIs kernel/main.c calls, a fake PP5022 register seam (hw/mmio.h
 * under -DMMIO_MOCK), fake time and the test baton. See headless.h for the
 * model and docs/testing-headless.md for how it is used.
 *
 * Each fake keeps the CONTRACT its hw header states, not the register
 * grammar (that is what tests/hw_mmio/ pins against the real drivers):
 *
 *   ata_*         512-byte LBA reads/writes on a disk-image file. Writes keep
 *                 the drive's physical-sector rule (ATA_PHYS_LOG: a write
 *                 must start and end on a 1 KB boundary, else IDNF) and may
 *                 be fenced to the pre-allocated files' extents. A parked or
 *                 slept drive spins up on the next access (hl_cfg.spinup_ms
 *                 of fake time).
 *   lcd_*         a panel buffer. Presents while slept are refused + counted.
 *   clickwheel_*  the tick sampler/drain split of hw/clickwheel.c: live state
 *                 for clickwheel_buttons(), a latch of down-edges and wheel
 *                 motion for clickwheel_get_event(), Hold edges first.
 *   hal_audio_*   a DAC that clocks at the stream rate in fake time and pulls
 *                 the player's source in 2048-frame "DMA buffers".
 *   i2c_*         a PCF50605: battery ADC (hl_cfg.battery_mv) and an RTC
 *                 that counts in fake time; the REAL hal/hw/battery.c and
 *                 hal/hw/rtc.c run on top of it.
 */

#include "headless.h"

#include "hal.h"
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/ata.h"
#include "hw/lcd.h"
#include "hw/clickwheel.h"
#include "hw/backlight.h"
#include "hw/headphone.h"
#include "hw/i2c.h"
#include "hw/power.h"
#include "hw/audio.h"
#include "hw/piezo.h"
#include "hw/uart.h"
#include "hw/volume.h"
#include "clock.h"
#include "timer.h"
#include "cache.h"
#include "sched.h"
#include "panic.h"
#include "evlog.h"
#include "datetime.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert((int)HL_BTN_SELECT == (int)WHEEL_BTN_SELECT, "button map");
_Static_assert((int)HL_BTN_MENU == (int)WHEEL_BTN_MENU, "button map");
_Static_assert((int)HL_BTN_PLAY == (int)WHEEL_BTN_PLAY, "button map");
_Static_assert((int)HL_BTN_LEFT == (int)WHEEL_BTN_LEFT, "button map");
_Static_assert((int)HL_BTN_RIGHT == (int)WHEEL_BTN_RIGHT, "button map");
_Static_assert(HL_W == LCD_WIDTH && HL_H == LCD_HEIGHT, "panel size");

/* The firmware's entry (kernel/main.c). */
_Noreturn void kernel_main(void);

hl_cfg_t hl_cfg = {
    .battery_mv     = 3950,
    .external_power = 0,
    .headphones     = 1,
    .hold           = 0,
    .spinup_ms      = 800,
    .standby_accept = 0,
    .rtc_epoch      = 1793534400u,      /* 2026-11-01 12:00:00 UTC */
    .verbose        = 0,
};

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("headless: FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(99);
}

/* ======================================================================
 * Time and the baton
 * ==================================================================== */

#define TICK_US 10000u                    /* the 100 Hz system tick */

static uint64_t g_now;                    /* fake microseconds since boot      */
static uint64_t g_until;                  /* hand back to the test at/after    */
static int      g_in_isr;                 /* inside a serviced "interrupt"     */
static hl_park_t g_park;

enum { TURN_TEST, TURN_FW };
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int             g_turn = TURN_TEST;
static pthread_t       g_fw_thread;
static int             g_booted;

static void fw_yield(void)
{
    pthread_mutex_lock(&g_mu);
    g_turn = TURN_TEST;
    pthread_cond_broadcast(&g_cv);
    while (g_turn != TURN_FW) {
        pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
}

static void test_hand_over(void)
{
    pthread_mutex_lock(&g_mu);
    g_turn = TURN_FW;
    pthread_cond_broadcast(&g_cv);
    while (g_turn != TURN_TEST) {
        pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
}

/* The firmware has stopped for good: give the baton back on every turn. */
static _Noreturn void park(hl_park_t why)
{
    g_park = why;
    for (;;) {
        fw_yield();
    }
}

static void audio_service(uint64_t dt_us);
static void tick_service(void);

/*
 * Move fake time forward by `us`, servicing the tick and the DAC on the way,
 * then hand the baton back if the test's slice is used up. Re-entrant calls
 * from inside a service (the player's source callback reading the clock)
 * only see the time; they do not move it.
 */
static void advance(uint64_t us)
{
    if (g_in_isr) {
        return;
    }
    uint64_t end = g_now + us;
    while (g_now < end) {
        uint64_t next_tick = (g_now / TICK_US + 1u) * TICK_US;
        uint64_t to = end < next_tick ? end : next_tick;
        uint64_t dt = to - g_now;
        g_now = to;
        g_in_isr = 1;
        audio_service(dt);
        if (g_now == next_tick) {
            tick_service();
        }
        g_in_isr = 0;
    }
    if (g_now >= g_until) {
        fw_yield();
    }
}

/* ======================================================================
 * Event timeline + UART log
 * ==================================================================== */

static hl_ev_t *g_ev;
static size_t   g_ev_n, g_ev_cap;

static void ev(int kind, uint32_t arg)
{
    if (g_ev_n == g_ev_cap) {
        g_ev_cap = g_ev_cap ? g_ev_cap * 2 : 4096;
        g_ev = realloc(g_ev, g_ev_cap * sizeof *g_ev);
        if (!g_ev) die("out of memory (events)");
    }
    g_ev[g_ev_n++] = (hl_ev_t){ g_now, kind, arg };
}

size_t hl_events_mark(void) { return g_ev_n; }
const hl_ev_t *hl_events(size_t *n) { *n = g_ev_n; return g_ev; }

static char  *g_log;
static size_t g_log_n, g_log_cap;

static void log_byte(char c)
{
    if (g_log_n + 2 > g_log_cap) {
        g_log_cap = g_log_cap ? g_log_cap * 2 : 65536;
        g_log = realloc(g_log, g_log_cap);
        if (!g_log) die("out of memory (log)");
    }
    g_log[g_log_n++] = c;
    g_log[g_log_n] = '\0';
    if (hl_cfg.verbose) {
        fputc(c, stdout);
        if (c == '\n') fflush(stdout);
    }
}

const char *hl_log(void) { return g_log ? g_log : ""; }
size_t      hl_log_len(void) { return g_log_n; }

/* ======================================================================
 * The register seam (hw/mmio.h, -DMMIO_MOCK)
 * ==================================================================== */

/* Every address nobody models reads back what was last written. */
#define REG_SLOTS 512
static struct { uintptr_t addr; uint32_t val; int used; } g_regs[REG_SLOTS];

static uint32_t *reg_slot(uintptr_t addr)
{
    size_t h = (size_t)((addr >> 2) * 2654435761u) % REG_SLOTS;
    for (size_t i = 0; i < REG_SLOTS; i++) {
        size_t k = (h + i) % REG_SLOTS;
        if (g_regs[k].used && g_regs[k].addr == addr) return &g_regs[k].val;
        if (!g_regs[k].used) {
            g_regs[k].used = 1;
            g_regs[k].addr = addr;
            g_regs[k].val  = 0;
            return &g_regs[k].val;
        }
    }
    die("register table full");
}

/* GPIOL: main charger present is active-LOW on bit 3, USB active-HIGH on bit
 * 4; GPIOB bit 0 is "charging", active-LOW (hal/hw/battery.c). The headless
 * external supply is USB. */
static uint32_t gpio_l_in(void)
{
    uint32_t v = 0x08u;                    /* main charger absent */
    if (hl_cfg.external_power) v |= 0x10u;
    return v;
}
static uint32_t gpio_b_in(void)
{
    int charging = hl_cfg.external_power && hl_cfg.battery_mv < 4150;
    return charging ? 0u : 0x01u;
}

uint32_t mmio_read32(uintptr_t addr)
{
    switch (addr) {
    case USEC_TIMER_ADDR:
        advance(1);                        /* a spin on the counter ends */
        return (uint32_t)g_now;
    case PROCESSOR_ID_ADDR:
        return 0x55u;                      /* the CPU core */
    case GPIOL_INPUT_VAL_ADDR:
        return gpio_l_in();
    case GPIOB_INPUT_VAL_ADDR:
        return gpio_b_in();
    default:
        return *reg_slot(addr);
    }
}

void mmio_write32(uintptr_t addr, uint32_t value)
{
    if (addr == CPU_CTL_ADDR && (value & PROC_WAIT_CNT)) {
        uint32_t n    = value & PROC_CNT_MASK;
        uint32_t unit = (value & PROC_CNT_MSEC) ? 1000u : 1u;
        advance((uint64_t)n * unit);
        return;
    }
    *reg_slot(addr) = value;
}

uint16_t mmio_read16(uintptr_t addr) { return (uint16_t)mmio_read32(addr); }
uint8_t  mmio_read8(uintptr_t addr)  { return (uint8_t)mmio_read32(addr); }
void mmio_write16(uintptr_t addr, uint16_t v) { *reg_slot(addr) = v; }
void mmio_write8(uintptr_t addr, uint8_t v)   { *reg_slot(addr) = v; }

/* ======================================================================
 * UART -> the log (and the event log's RAM tap, as hw/uart.c does)
 * ==================================================================== */

void uart_init(void) {}
void uart_putc(char c) { evlog_capture((uint8_t)c); log_byte(c); }
void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
void uart_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_putc(hex[(v >> shift) & 0xF]);
    }
}
void uart_clock_suspend(void) {}
void uart_clock_resume(void) {}

#ifndef CORE_RELEASE
void log_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uart_puts(buf);
    uart_putc('\n');
}
#endif

/* ======================================================================
 * LCD (BCM) -> a panel buffer
 * ==================================================================== */

static uint16_t g_panel[LCD_WIDTH * LCD_HEIGHT];
static int      g_lcd_slept;
static uint32_t g_pres_full, g_pres_rect, g_pres_refused;

int lcd_init(void) { return 1; }           /* the BCM rail reads powered */

void lcd_fill(uint16_t rgb565)
{
    for (size_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) g_panel[i] = rgb565;
}

void lcd_present_fb(const uint16_t *fb)
{
    if (g_lcd_slept) {
        g_pres_refused++;
        ev(HL_EV_PRESENT_REFUSED, 0);
        return;
    }
    memcpy(g_panel, fb, sizeof g_panel);
    g_pres_full++;
    ev(HL_EV_PRESENT_FULL, 0);
}

void lcd_present_rect(const uint16_t *fb, int x, int y, int w, int h)
{
    if (g_lcd_slept) {
        g_pres_refused++;
        ev(HL_EV_PRESENT_REFUSED, 1);
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int r = y; r < y + h; r++) {
        memcpy(&g_panel[r * LCD_WIDTH + x], &fb[r * LCD_WIDTH + x],
               (size_t)w * sizeof(uint16_t));
    }
    g_pres_rect++;
    ev(HL_EV_PRESENT_RECT, ((uint32_t)x << 16) | (uint32_t)y);
}

void lcd_sleep(void) { if (!g_lcd_slept) ev(HL_EV_LCD_SLEEP, 0); g_lcd_slept = 1; }
void lcd_wake(void)  { if (g_lcd_slept) ev(HL_EV_LCD_WAKE, 0);  g_lcd_slept = 0; }
int  lcd_is_slept(void) { return g_lcd_slept; }
uint32_t lcd_presents_refused(void) { return g_pres_refused; }
uint32_t lcd_bcm_timeouts(void) { return 0; }
int  bcm_init(void) { return LCD_BCM_OK; }
int  lcd_recover(void) { return LCD_BCM_OK; }
uint32_t lcd_bcm_recoveries(void) { return 0; }

const uint16_t *hl_panel(void) { return g_panel; }

int hl_write_bmp(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    const int W = LCD_WIDTH, H = LCD_HEIGHT;
    const int stride = W * 3;              /* 960: already a multiple of 4 */
    const uint32_t pix = (uint32_t)(stride * H), fsz = 54u + pix;
    uint8_t hd[54] = { 'B', 'M' };
    hd[2] = (uint8_t)fsz; hd[3] = (uint8_t)(fsz >> 8);
    hd[4] = (uint8_t)(fsz >> 16); hd[5] = (uint8_t)(fsz >> 24);
    hd[10] = 54; hd[14] = 40;
    hd[18] = (uint8_t)W; hd[19] = (uint8_t)(W >> 8);
    int32_t nh = -H;
    memcpy(&hd[22], &nh, 4);
    hd[26] = 1; hd[28] = 24;
    memcpy(&hd[34], &pix, 4);
    fwrite(hd, 1, sizeof hd, fp);
    uint8_t row[LCD_WIDTH * 3];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t p = g_panel[y * W + x];
            row[x * 3 + 0] = (uint8_t)((p & 0x1F) << 3);
            row[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3F) << 2);
            row[x * 3 + 2] = (uint8_t)(((p >> 11) & 0x1F) << 3);
        }
        fwrite(row, 1, sizeof row, fp);
    }
    return fclose(fp) == 0 ? 0 : -1;
}

int lcd_screenshot_bmp(const char *path) { return hl_write_bmp(path); }

/* ======================================================================
 * Backlight, piezo, headphone jack, power
 * ==================================================================== */

static int g_bl;

void backlight_init(void) { g_bl = BACKLIGHT_MAX; ev(HL_EV_BACKLIGHT, (uint32_t)g_bl); }
void backlight_set(int level)
{
    if (level < 0) level = 0;
    if (level > BACKLIGHT_MAX) level = BACKLIGHT_MAX;
    if (level != g_bl) ev(HL_EV_BACKLIGHT, (uint32_t)level);
    g_bl = level;
}
void backlight_service(void) {}

static uint32_t g_clicks;
void piezo_init(void) {}
void piezo_click(void) { g_clicks++; }
void piezo_click_ex(uint32_t hz, uint32_t us) { (void)hz; (void)us; g_clicks++; }
void piezo_clock_suspend(void) {}
void piezo_clock_resume(void) {}

int headphone_raw(void) { return hl_cfg.headphones ? 1 : 0; }
int headphone_pin_cfg(void) { return HEADPHONE_PIN_ENABLED; }
int hal_headphones_present(void) { return hl_cfg.headphones ? 1 : 0; }

static uint32_t g_pmu_standby;
int power_standby(void)
{
    g_pmu_standby++;
    ev(HL_EV_PMU_STANDBY, (uint32_t)hl_cfg.standby_accept);
    uart_puts("headless: power_standby\n");
    if (hl_cfg.standby_accept) {
        park(HL_PARK_POWEROFF);
    }
    return -1;                              /* the PMU "refused" */
}
_Noreturn void power_reboot(void) { park(HL_PARK_REBOOT); }
_Noreturn void power_enter_disk_mode(void) { park(HL_PARK_DISKMODE); }

/* ======================================================================
 * I2C: a PCF50605 (battery ADC + RTC). Anything else on the bus (the
 * WM8758) is accepted and ignored.
 * ==================================================================== */

#define PMU_ADDR    0x08
#define PMU_RTCSC   0x0A
#define PMU_RTCYR   0x10
#define PMU_ADCC1   0x2F
#define PMU_ADCS1   0x30

static uint8_t  g_pmu[256];
static uint32_t g_rtc_epoch0;               /* RTC reading at g_rtc_t0       */
static uint64_t g_rtc_t0;

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

static void rtc_regs_refresh(void)
{
    uint32_t e = g_rtc_epoch0 + (uint32_t)((g_now - g_rtc_t0) / 1000000u);
    datetime_t d;
    if (!datetime_from_epoch(e, &d)) {
        return;
    }
    g_pmu[0x0A] = bcd(d.sec);
    g_pmu[0x0B] = bcd(d.min);
    g_pmu[0x0C] = bcd(d.hour);
    g_pmu[0x0D] = (uint8_t)d.wday;
    g_pmu[0x0E] = bcd(d.day);
    g_pmu[0x0F] = bcd(d.month);
    g_pmu[0x10] = bcd(d.year - 2000);
}

static void rtc_regs_latch(void)
{
    datetime_t d = {
        .year = 2000 + unbcd(g_pmu[0x10]), .month = unbcd(g_pmu[0x0F]),
        .day = unbcd(g_pmu[0x0E]), .hour = unbcd(g_pmu[0x0C]),
        .min = unbcd(g_pmu[0x0B]), .sec = unbcd(g_pmu[0x0A]),
        .wday = g_pmu[0x0D],
    };
    if (datetime_valid(&d)) {
        g_rtc_epoch0 = datetime_to_epoch(&d);
        g_rtc_t0     = g_now;
    }
}

void i2c_init(void) {}
void i2c_clock_suspend(void) {}
void i2c_clock_resume(void) {}

int i2c_send(uint8_t dev, const uint8_t *bytes, int len)
{
    if (dev != PMU_ADDR || len < 1) {
        return 0;
    }
    uint8_t reg = bytes[0];
    int rtc = 0;
    if (reg >= PMU_RTCSC && reg <= PMU_RTCYR) {
        rtc_regs_refresh();                 /* a partial write keeps the rest */
    }
    for (int i = 1; i < len; i++, reg++) {
        g_pmu[reg] = bytes[i];
        if (reg >= PMU_RTCSC && reg <= PMU_RTCYR) rtc = 1;
        if (reg == PMU_ADCC1) {
            /* A conversion: the result is ready at once. 10 bits over a
             * 6000 mV full scale (hal/hw/battery.c). */
            int raw = (hl_cfg.battery_mv * 1024) / 6000;
            if (raw > 1023) raw = 1023;
            if (raw < 0) raw = 0;
            g_pmu[PMU_ADCS1]     = (uint8_t)(raw >> 2);
            g_pmu[PMU_ADCS1 + 1] = (uint8_t)(raw & 3);
        }
    }
    if (rtc) {
        rtc_regs_latch();
    }
    return 0;
}

int i2c_read(uint8_t dev, uint8_t reg, uint8_t *buf, int n)
{
    if (dev != PMU_ADDR) {
        memset(buf, 0, (size_t)n);
        return 0;
    }
    rtc_regs_refresh();
    for (int i = 0; i < n; i++) {
        buf[i] = g_pmu[(uint8_t)(reg + i)];
    }
    return 0;
}

/* ======================================================================
 * ATA: a disk image
 * ==================================================================== */

static int      g_disk_fd = -1;
static uint64_t g_disk_bytes;
static int      g_ata_parked, g_ata_slept;
static uint32_t g_ata_reads, g_ata_writes, g_ata_spinups, g_bad_writes;

#define MAX_RANGES 64
static struct { uint32_t lba, count; } g_allow[MAX_RANGES];
static int g_allow_n;

int hl_allow_writes(uint32_t lba, uint32_t count)
{
    if (g_allow_n == MAX_RANGES) return -1;
    g_allow[g_allow_n].lba   = lba;
    g_allow[g_allow_n].count = count;
    g_allow_n++;
    return 0;
}

static void ata_spin_up(void)
{
    if (!g_ata_parked) return;
    g_ata_parked = g_ata_slept = 0;
    g_ata_spinups++;
    ev(HL_EV_ATA_SPINUP, 0);
    advance((uint64_t)hl_cfg.spinup_ms * 1000u);
}

int ata_init(void) { return g_disk_fd >= 0 ? 0 : -1; }

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf)
{
    if (((uintptr_t)buf & 1u) != 0) die("ata_read_sectors: unaligned buffer");
    if ((uint64_t)lba + count > g_disk_bytes / 512u) return -1;
    ata_spin_up();
    g_ata_reads++;
    size_t want = (size_t)count * 512u;
    ssize_t got = pread(g_disk_fd, buf, want, (off_t)lba * 512);
    if (got != (ssize_t)want) return -2;
    return 0;
}

int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf)
{
    if ((lba % ATA_PHYS_LOG) != 0 || (count % ATA_PHYS_LOG) != 0 || count == 0) {
        g_bad_writes++;
        uart_puts("headless: MISALIGNED ata write\n");
        return ATA_ERR_IDNF;
    }
    if ((uint64_t)lba + count > g_disk_bytes / 512u) return -1;
    if (g_allow_n > 0) {
        int ok = 0;
        for (int i = 0; i < g_allow_n; i++) {
            if (lba >= g_allow[i].lba &&
                (uint64_t)lba + count <= (uint64_t)g_allow[i].lba + g_allow[i].count) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            g_bad_writes++;
            fprintf(stderr, "headless: write OUTSIDE the pre-allocated files: "
                    "lba %u count %u\n", lba, count);
            return -1;
        }
    }
    ata_spin_up();
    g_ata_writes++;
    ev(HL_EV_ATA_WRITE, lba);
    size_t want = (size_t)count * 512u;
    if (pwrite(g_disk_fd, buf, want, (off_t)lba * 512) != (ssize_t)want) return -2;
    return 0;
}

int ata_identify(void *buf) { memset(buf, 0, 512); return 0; }

int ata_standby(void)
{
    ev(HL_EV_ATA_STANDBY, 0);
    g_ata_parked = 1;
    return 0;
}

int ata_sleep(void)
{
    ev(HL_EV_ATA_SLEEP, 0);
    g_ata_parked = g_ata_slept = 1;
    return 0;
}

int ata_wakeup(void)
{
    ev(HL_EV_ATA_WAKEUP, 0);
    ata_spin_up();
    return 0;
}

int ata_is_parked(void) { return g_ata_parked; }
int ata_is_slept(void)  { return g_ata_slept; }

/* ======================================================================
 * Click wheel: a scripted input queue sampled on the 100 Hz tick
 * ==================================================================== */

enum { IN_DOWN, IN_UP, IN_WHEEL, IN_UNTOUCH, IN_HOLD };
typedef struct { uint64_t t; int kind; int val; } in_ev_t;

#define IN_MAX 1024
static in_ev_t g_in[IN_MAX];
static int     g_in_n;

static int     g_cw_armed;
static int     g_hold;
static uint8_t g_btn_live, g_latch_btn;
static int     g_latch_delta;
static int     g_touched;
static int     g_hold_edge;

static void in_push(uint64_t t, int kind, int val)
{
    if (g_in_n == IN_MAX) die("input queue full");
    g_in[g_in_n++] = (in_ev_t){ t, kind, val };
}

/* The tick: apply every scripted input that is due, in order. */
static void tick_service(void)
{
    int w = 0;
    for (int i = 0; i < g_in_n; i++) {
        in_ev_t e = g_in[i];
        if (e.t > g_now) {
            g_in[w++] = e;
            continue;
        }
        switch (e.kind) {
        case IN_DOWN:
            if (!g_hold) {
                g_btn_live  |= (uint8_t)e.val;
                g_latch_btn |= (uint8_t)e.val;
            }
            break;
        case IN_UP:
            g_btn_live &= (uint8_t)~e.val;
            break;
        case IN_WHEEL:
            if (!g_hold) {
                g_latch_delta += e.val;
                if (g_latch_delta > 127) g_latch_delta = 127;
                if (g_latch_delta < -128) g_latch_delta = -128;
                g_touched = 1;
            }
            break;
        case IN_UNTOUCH:
            g_touched = 0;
            break;
        case IN_HOLD:
            if (e.val != g_hold) {
                g_hold = e.val;
                g_hold_edge = 1;
                if (g_hold) {               /* OPTO gated: nothing reads */
                    g_btn_live = g_latch_btn = 0;
                    g_latch_delta = 0;
                }
            }
            break;
        }
    }
    g_in_n = w;
}

void clickwheel_init(void) { g_cw_armed = 1; }
bool clickwheel_poll(wheel_event_t *e) { return clickwheel_get_event(e); }
void clickwheel_service(void) {}

bool clickwheel_get_event(wheel_event_t *e)
{
    if (!e || !g_cw_armed) return false;
    if (g_hold_edge) {
        g_hold_edge = 0;
        g_latch_btn = 0;
        *e = (wheel_event_t){ .hold = g_hold != 0 };
        return true;
    }
    if (g_latch_btn == 0 && g_latch_delta == 0) return false;
    e->buttons     = g_latch_btn;
    e->wheel_delta = (int8_t)g_latch_delta;
    e->touched     = g_touched != 0;
    e->hold        = false;
    g_latch_btn = 0;
    g_latch_delta = 0;
    return true;
}

bool    clickwheel_hold(void) { return g_hold != 0; }
uint8_t clickwheel_buttons(void) { return g_hold ? 0 : g_btn_live; }

/* ======================================================================
 * Audio: the DAC clocks in fake time and pulls the source per DMA buffer
 * ==================================================================== */

#define DMA_FRAMES 2048

static audio_source_fn g_src;
static void           *g_src_user;
static int      g_a_open, g_a_running, g_a_cold;
static uint32_t g_a_rate;
static uint16_t g_a_ch;
static uint64_t g_a_frac;                  /* frames * 1e6, carried         */
static uint32_t g_a_left;                  /* frames left in the "buffer"   */
static uint32_t g_a_stream_played;         /* since hal_audio_init          */
static uint64_t g_a_played, g_a_pulled, g_a_nonzero;
static uint32_t g_a_underruns, g_a_underruns_total;
static int      g_volume = 70, g_balance;

static void dma_refill(void)
{
    static int16_t buf[DMA_FRAMES * 2];
    int got = 0;
    if (g_src) {
        got = g_src(g_src_user, buf, DMA_FRAMES);
        if (got < 0) got = 0;
        if (got > DMA_FRAMES) got = DMA_FRAMES;
    }
    if (got < DMA_FRAMES) { g_a_underruns++; g_a_underruns_total++; }
    g_a_pulled += (uint64_t)got;
    for (int i = 0; i < got * g_a_ch; i++) {
        if (buf[i] != 0) g_a_nonzero++;
    }
    g_a_left = DMA_FRAMES;                  /* the padding plays too */
}

static void audio_service(uint64_t dt_us)
{
    if (!g_a_open || !g_a_running || g_a_cold) return;
    g_a_frac += dt_us * g_a_rate;
    uint64_t n = g_a_frac / 1000000u;
    g_a_frac -= n * 1000000u;
    while (n > 0) {
        if (g_a_left == 0) dma_refill();
        uint32_t take = n < g_a_left ? (uint32_t)n : g_a_left;
        g_a_left -= take;
        g_a_stream_played += take;
        g_a_played += take;
        n -= take;
    }
}

int hal_audio_init(uint32_t rate, uint16_t channels)
{
    if (channels != 1 && channels != 2) return -1;
    if (rate != 44100 && rate != 48000 && rate != 32000 && rate != 24000 &&
        rate != 22050) {
        return -1;
    }
    g_a_open = 1;
    g_a_running = 0;
    g_a_cold = 0;
    g_a_rate = rate;
    g_a_ch = channels;
    g_a_frac = 0;
    g_a_left = 0;
    g_a_stream_played = 0;
    g_a_underruns = 0;                      /* per stream, as hw/audio.c */
    ev(HL_EV_AUDIO_INIT, rate);
    return 0;
}

void hal_audio_set_source(audio_source_fn fn, void *user) { g_src = fn; g_src_user = user; }

void hal_audio_start(void)
{
    if (!g_a_open) return;
    if (!g_a_running) ev(HL_EV_AUDIO_START, 0);
    g_a_running = 1;
}

void hal_audio_stop(void)
{
    if (g_a_running) ev(HL_EV_AUDIO_STOP, 0);
    g_a_running = 0;
}

void hal_audio_flush(void) { g_a_left = 0; }
uint32_t hal_audio_frames_played(void) { return g_a_open ? g_a_stream_played : 0; }

void hal_audio_close(void)
{
    if (g_a_open) ev(HL_EV_AUDIO_CLOSE, 0);
    g_a_open = g_a_running = 0;
    g_src = NULL;
    g_src_user = NULL;
}

int hal_audio_drain(uint32_t timeout_ms)
{
    if (!g_a_open || !g_a_running || g_a_left == 0) return 0;
    uint64_t us = ((uint64_t)g_a_left * 1000000u + g_a_rate - 1) / g_a_rate;
    if (us > (uint64_t)timeout_ms * 1000u) {
        advance((uint64_t)timeout_ms * 1000u);
        return -1;
    }
    advance(us);
    return 0;
}

void hal_audio_suspend(void)
{
    if (g_a_running || g_a_cold) return;
    g_a_cold = 1;
    ev(HL_EV_AUDIO_SUSPEND, 0);
}
void hal_audio_boot_quiet(void) { g_a_cold = 1; }
int hal_audio_wake(void)
{
    if (g_a_cold) ev(HL_EV_AUDIO_WAKE, 0);
    g_a_cold = 0;
    return 0;
}

void     audio_dma_isr(void) {}
uint32_t audio_dma_completions(void) { return (uint32_t)(g_a_played / DMA_FRAMES); }
uint32_t audio_underruns(void) { return g_a_underruns; }
uint32_t audio_late_kicks(void) { return 0; }
uint32_t audio_late_worst_us(void) { return 0; }
void     audio_fifo_service(void) {}
uint32_t audio_fifo_empty_samples(void) { return 0; }
uint32_t audio_fifo_worst_free(void) { return 0; }

void hal_volume_init(void) { g_volume = 70; }
void hal_volume_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_volume = percent;
}
int  hal_volume_get(void) { return g_volume; }
void hal_balance_set(int balance) { g_balance = balance; }
int  hal_balance_get(void) { return g_balance; }
void hal_eq_set(const int8_t gain_db[EQ_BAND_COUNT],
                const uint8_t cutoff[EQ_BAND_COUNT],
                const uint8_t narrow[EQ_BAND_COUNT])
{
    (void)gain_db; (void)cutoff; (void)narrow;
}
void hal_codec_restore(void) {}
uint16_t hal_volume_out1_word(int percent) { return (uint16_t)percent; }

/* ======================================================================
 * The kernel's hardware-bound half: clock.c, timer.c, cache.c, sched.c,
 * panic.c (each of those programs PP5022 blocks or switches ARM contexts)
 * ==================================================================== */

static int      g_boost;
static uint32_t g_freq = CPUFREQ_DEFAULT;
static int      g_parked_pll;

void clock_init(void) { g_freq = CPUFREQ_NORMAL; }
void cpu_boost(void)
{
    if (g_boost++ == 0) g_freq = CPUFREQ_MAX;
    g_parked_pll = 0;
    ev(HL_EV_CPU_BOOST, (uint32_t)g_boost);
}
void cpu_unboost(void)
{
    if (g_boost > 0 && --g_boost == 0) g_freq = CPUFREQ_NORMAL;
    ev(HL_EV_CPU_UNBOOST, (uint32_t)g_boost);
}
uint32_t cpu_frequency(void) { return g_freq; }
int  clock_suspend(void)
{
    if (g_boost > 0) return -1;
    g_parked_pll = 1;
    g_freq = CPUFREQ_DEFAULT;
    return 0;
}
void clock_resume(void) { if (g_parked_pll) { g_parked_pll = 0; g_freq = CPUFREQ_NORMAL; } }
void clock_gate_suspend(void) {}
void clock_gate_resume(void) {}
void clock_set_audio_dma_active(int active) { (void)active; }

void timer_init(void) {}
void timer_set_rate(uint32_t hz) { (void)hz; }
void timer_tick_isr(void) {}
uint32_t current_tick(void) { return (uint32_t)(g_now / TICK_US); }
void sleep_ms(uint32_t ms) { advance((uint64_t)ms * 1000u); }

void cache_init(void) {}
void cache_commit(void) {}

void sched_init(void) {}
int  sched_add_task(task_fn_t entry, void *stack_base, size_t stack_size,
                    const char *name)
{
    (void)entry; (void)stack_base; (void)stack_size; (void)name;
    return 0;
}
void sched_start(void) { park(HL_PARK_IDLE_FALLBACK); }
/* NOT sched_yield(): on a host that is POSIX's, and libc / the sanitizer
 * runtimes call it from inside their own locks — a fake that parks there
 * deadlocks the process. main.c's only caller is idle_task, which runs only
 * under sched_start(), and sched_start() above never returns. */
int  sched_current(void) { return 0; }
int  sched_pick_next(void) { return 0; }

_Noreturn void panic(uint32_t cause, const panic_regs_t *regs)
{
    (void)regs;
    fprintf(stderr, "headless: firmware PANIC cause %u\n", cause);
    park(HL_PARK_PANIC);
}
int  stack_guard_breached(void) { return 0; }
void stack_stat(stack_stat_t *st) { memset(st, 0, sizeof *st); }

/* ======================================================================
 * The test side
 * ==================================================================== */

static void *fw_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mu);
    while (g_turn != TURN_FW) {
        pthread_cond_wait(&g_cv, &g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    kernel_main();
}

int hl_boot(const char *disk, uint32_t ms)
{
    if (g_booted) die("hl_boot called twice");
    const char *v = getenv("HL_VERBOSE");
    if (v && atoi(v)) hl_cfg.verbose = 1;

    g_disk_fd = open(disk, O_RDWR);
    if (g_disk_fd < 0) {
        fprintf(stderr, "headless: cannot open %s: %s\n", disk, strerror(errno));
        return -1;
    }
    off_t end = lseek(g_disk_fd, 0, SEEK_END);
    if (end <= 0) return -1;
    g_disk_bytes = (uint64_t)end;

    g_hold       = hl_cfg.hold;
    g_rtc_epoch0 = hl_cfg.rtc_epoch;
    g_rtc_t0     = 0;
    g_booted     = 1;

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    if (pthread_create(&g_fw_thread, &at, fw_main, NULL) != 0) return -1;
    pthread_attr_destroy(&at);
    hl_run_ms(ms);
    return 0;
}

void hl_run_ms(uint32_t ms)
{
    if (!g_booted || g_park != HL_RUNNING) return;
    g_until = g_now + (uint64_t)ms * 1000u;
    test_hand_over();
}

int hl_run_until(int (*pred)(void *), void *arg, uint32_t step_ms, uint32_t max_ms)
{
    uint32_t spent = 0;
    while (!pred(arg)) {
        if (spent >= max_ms || g_park != HL_RUNNING) return 0;
        hl_run_ms(step_ms);
        spent += step_ms;
    }
    return 1;
}

void hl_press(unsigned btn, uint32_t hold_ms)
{
    in_push(g_now, IN_DOWN, (int)btn);
    in_push(g_now + (uint64_t)hold_ms * 1000u, IN_UP, (int)btn);
}

void hl_scroll(int rows)
{
    int dir = rows < 0 ? -1 : 1;
    int n = rows < 0 ? -rows : rows;
    uint64_t t = g_now;
    for (int i = 0; i < n; i++, t += 150000u) {
        in_push(t, IN_WHEEL, dir * 4);      /* WHEEL_CLICKS_PER_ITEM */
    }
    in_push(t + 100000u, IN_UNTOUCH, 0);
}

void hl_tap(unsigned btn, uint32_t settle_ms)
{
    hl_press(btn, 60);
    hl_run_ms(60 + settle_ms);
}

void hl_set_hold(int on) { in_push(g_now, IN_HOLD, on ? 1 : 0); }
void hl_set_headphones(int present) { hl_cfg.headphones = present ? 1 : 0; }
void hl_set_external_power(int on) { hl_cfg.external_power = on ? 1 : 0; }
void hl_set_battery_mv(int mv) { hl_cfg.battery_mv = mv; }

hl_park_t hl_parked(void) { return g_park; }

hl_stats_t hl_stats(void)
{
    hl_stats_t s = {
        .now_us = g_now,
        .presents_full = g_pres_full, .presents_rect = g_pres_rect,
        .presents_refused = g_pres_refused,
        .lcd_slept = g_lcd_slept,
        .backlight = g_bl,
        .ata_reads = g_ata_reads, .ata_writes = g_ata_writes,
        .ata_spinups = g_ata_spinups, .bad_writes = g_bad_writes,
        .ata_parked = g_ata_parked, .ata_slept = g_ata_slept,
        .audio_open = g_a_open, .audio_running = g_a_running,
        .audio_cold = g_a_cold, .audio_rate = g_a_rate,
        .audio_frames_played = g_a_played, .audio_frames_pulled = g_a_pulled,
        .audio_nonzero = g_a_nonzero, .audio_underruns = g_a_underruns,
        .audio_underruns_total = g_a_underruns_total,
        .cpu_boost = g_boost,
        .piezo_clicks = g_clicks,
        .pmu_standby_calls = g_pmu_standby,
        .volume = g_volume,
    };
    return s;
}
