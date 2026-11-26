/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/boot_order_test.c — ui_boot()'s order, judged by what a boot
 * of a disk with a saved setting SHOWS.
 *
 * ui_boot() (kernel/main.c) is run_ui's prologue, and its comments say the
 * order is load-bearing. Part of that order is held elsewhere: settings
 * defaults before the load (settings-leave, firmware-walk), the library and
 * the On-The-Go list before the resume and the shuffle deal pushed before it
 * (resume-context). This holds the steps whose wrong order leaves the
 * device's END state right — settings_apply() re-applies the theme and the
 * charger a moment later, the loop repaints anyway — and is visible only in
 * the frames the boot paints on the way, or on the NEXT boot:
 *
 *   - config_load() before the library "so the saved theme is known before
 *     anything paints a loading screen", and theme_set() before the
 *     "LOADING" repaint: the loading frames come up in the saved theme;
 *   - charger_apply() before the library load: the Charge Rate budget is
 *     asked for through the load, not the LTC4066's 100 mA reset state;
 *   - battery_refresh(1) before the loop seeds its "gauge as drawn": the
 *     first pass paints the main menu once, not twice;
 *   - the clock mark re-armed AFTER cfg_commit_clear(): a stamp acted on
 *     whose forced write failed is still written, and not acted on again.
 *
 *   session save    Settings > Theme > Onyx (SELECT applies it), leave
 *                   Settings, wait for "cfg save rc 0"; power cut.
 *   session boot    power on, every full present captured as it happens
 *                   (hl_on_present). The ONE loading frame painted before the
 *                   record is read (kernel_main's) is Linen — the default,
 *                   and the proof this test can see a wrong-theme frame.
 *                   Every loading frame painted after "core: cfg load" is
 *                   Onyx with the charger at 500 mA, there are at least two
 *                   ("LOADING", "LOADING LIBRARY"), and one frame follows
 *                   them in the boot: the main menu.
 *   session stamp   a second disk whose CORECFG.DAT carries a host stamp a
 *                   day ahead of the RTC; the boot's first ATA write is
 *                   refused (hl_cfg.fail_writes). The stamp sets the clock,
 *                   the forced write of the mark fails, the mark is re-armed
 *                   and the idle path writes it. Power cut.
 *   session stamp-next  power on: applied == host, "-> none".
 *
 * Throughout: no write outside the pre-allocated files, no panic.
 *
 * Usage: boot_order_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "hw/battery.h"             /* charger_max_current(), read-only     */
#include "kernel/config.h"          /* config_encode(): the stamped record  */
#include "ui/settings.h"            /* settings_defaults()                  */
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "boot-order", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* The two themes' surface colours (ui/palette.c PAL_LINEN / PAL_ONYX
 * [PAL_SURFACE]): what console_clear() fills the boot screen with. */
#define SURFACE_LINEN 0xF79Du
#define SURFACE_ONYX  0x18C2u

/* ---- helpers (as settings_leave_test.c's) -------------------------------- */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
/* The cursor row is the one drawn the other way up from the surface: light
 * on a dark bar in Linen, dark on a light bar in Onyx. The surface is read
 * off the bottom-left pixel, below every row. */
static int selected(const char *s)
{
    hl_text_hit_t h;
    int dark = lum(hl_panel()[HL_W * (HL_H - 1)]) < lum(0x7BEFu);
    return hl_find_text_in(hl_panel(), s, 40, HL_H, &h) &&
           (dark ? lum(h.ink) < lum(h.bg) : lum(h.ink) > lum(h.bg));
}
static int select_row(const char *s)
{
    if (selected(s)) return 1;
    hl_scroll(-16);
    hl_run_ms(16 * 150 + 400);
    for (int i = 0; i < 24; i++) {
        if (selected(s)) return 1;
        hl_scroll(1);
        hl_run_ms(450);
    }
    return selected(s);
}
static void tap(unsigned btn) { hl_tap(btn, 500); }
static void to_main_menu(void)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
}

static int log_has(const char *s) { return strstr(hl_log(), s) != NULL; }
static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
}
typedef struct { size_t from; const char *needle; } log_wait_t;
static int pred_log(void *a)
{
    const log_wait_t *w = a;
    return log_has_since(w->from, w->needle);
}

static int copy_disk(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (in < 0 || out < 0) return -1;
    static char buf[1 << 16];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) return -1;
    }
    close(in);
    return close(out);
}
static int load_ranges(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned lba, cnt;
    char name[512];
    int n = 0;
    while (fscanf(f, "%u %u %511[^\n]", &lba, &cnt, name) == 3) {
        if (hl_allow_writes(lba, cnt) == 0) n++;
    }
    fclose(f);
    return n;
}

static void boot(const char *disk, const char *ranges)
{
    CHECK("image: the pre-allocated files' extents are known", load_ranges(ranges) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        _exit(2);
    }
    CHECK("boot: reached the browser", log_has("core: entering browser"));
}

/* A hard power cut: the firmware thread is frozen mid-wait and simply stops
 * existing. */
static _Noreturn void power_cut(const char *what)
{
    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: power cut at fake time %.1f s\n", what, (double)s.now_us / 1e6);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}

/* Music > Albums > Long Player: a folder read, which spins a parked drive
 * up (and a pending settings change lands as the platters turn). */
static int open_album(void)
{
    to_main_menu();
    int ok = select_row("Music");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Albums");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Long Player");
    tap(HL_BTN_SELECT);
    return ok;
}

/* ---- the frames the boot presents ----------------------------------------- */

/* Every full present of the boot, captured on the firmware thread as it
 * happens (the panel moves on long before a test could look), each with
 * whether the settings record had been read by then. Whether it is a loading
 * screen is decided there too: hl_find_text() matches ink against the LIVE
 * palette, which by the time the test thread looks is the saved theme's, not
 * the one a default-theme frame was drawn in. */
#define MAX_FRAMES 24
typedef struct {
    int      after_cfg;            /* "core: cfg load" was already printed */
    int      loading;              /* "LOADING" / "LOADING LIBRARY" on it  */
    int      charger_ma;           /* the budget asked of the LTC4066 then */
    uint16_t px[HL_W * HL_H];
} frame_t;
static frame_t g_frames[MAX_FRAMES];
static int     g_nframes, g_dropped;

static int is_loading(const uint16_t *px);

static void on_present(void *user)
{
    (void)user;
    if (g_nframes >= MAX_FRAMES) {
        g_dropped++;
        return;
    }
    frame_t *f = &g_frames[g_nframes++];
    f->after_cfg = strstr(hl_log(), "core: cfg load") != NULL;
    f->charger_ma = charger_max_current();
    memcpy(f->px, hl_panel(), sizeof f->px);
    f->loading = is_loading(f->px);
}

static int is_loading(const uint16_t *px)
{
    return hl_find_text(px, "LOADING", NULL) ||
           hl_find_text(px, "LOADING LIBRARY", NULL);
}

/* ---- the sessions ---------------------------------------------------------- */

static _Noreturn void s_save(const char *disk, const char *ranges)
{
    T.suite = "boot-order(save)";
    boot(disk, ranges);
    to_main_menu();
    CHECK("save: Settings > Theme",
          select_row("Settings") && (tap(HL_BTN_SELECT), select_row("Theme")));
    tap(HL_BTN_SELECT);
    /* The picker previews the theme under the cursor, so in a dark one an
     * unselected row is light-on-dark and select_row() cannot tell it from
     * the cursor. Go by position: the top row is Linen, Onyx the next. */
    hl_scroll(-16);
    hl_run_ms(16 * 150 + 400);
    hl_scroll(1);
    hl_run_ms(450);
    CHECK("save: the cursor on Onyx, previewed",
          hl_find_text(hl_panel(), "Onyx", NULL) &&
          hl_panel()[HL_W * (HL_H - 1)] == SURFACE_ONYX);
    size_t mark = hl_log_len();
    tap(HL_BTN_SELECT);                                 /* apply, for real */
    tap(HL_BTN_MENU);                                   /* Theme -> root   */
    tap(HL_BTN_MENU);                                   /* leave Settings  */
    CHECK("save: the main menu is drawn in Onyx",
          selected("Settings") && hl_panel()[HL_W * (HL_H - 1)] == SURFACE_ONYX);
    /* Written at the leave if the platters turn, else at the next spin-up:
     * a folder read gives it one. */
    log_wait_t w = { mark, "core: cfg save rc 00000000 seq " };
    int saved = hl_run_until(pred_log, &w, 250, 4000);
    if (!saved) {
        to_main_menu();
        saved = open_album() && hl_run_until(pred_log, &w, 250, 10000);
    }
    CHECK("save: the theme reaches the disk (cfg save rc 0)", saved);
    power_cut("save");
}

static _Noreturn void s_boot(const char *disk, const char *ranges)
{
    T.suite = "boot-order(boot)";
    hl_on_present(on_present, NULL);
    boot(disk, ranges);
    hl_on_present(NULL, NULL);

    int before = 0, before_linen = 0, after = 0, after_onyx = 0, after_500 = 0;
    int last_loading = -1;
    for (int i = 0; i < g_nframes; i++) {
        const frame_t *f = &g_frames[i];
        if (!f->loading) continue;
        last_loading = i;
        uint16_t surface = f->px[0];                    /* top-left: surface */
        if (!f->after_cfg) {
            before++;
            before_linen += surface == SURFACE_LINEN;
            continue;
        }
        after++;
        after_onyx += surface == SURFACE_ONYX;
        after_500  += f->charger_ma == 500;
        if (surface != SURFACE_ONYX || f->charger_ma != 500) {
            printf("boot: loading frame %d (after cfg load): surface %04X "
                   "(Onyx is %04X), charger %d mA\n", i, surface,
                   SURFACE_ONYX, f->charger_ma);
        }
    }
    int ui_frames = g_nframes - 1 - last_loading;
    printf("boot: %d full presents (%d dropped); loading frames: %d before "
           "the record was read, %d after; %d after the last of them\n",
           g_nframes, g_dropped, before, after, ui_frames);
    CHECK("boot: the saved record was loaded", log_has("core: cfg load 00000001"));

    /* The theme: config_load() before the library, theme_set() before the
     * repaint. */
    CHECK("boot: the frame painted before the record is read is the default, Linen",
          before == 1 && before_linen == 1);
    CHECK("boot: loading frames after the record is read: LOADING and "
          "LOADING LIBRARY at least", after >= 2);
    CHECK("boot: ...every one of them in the saved theme, Onyx",
          after > 0 && after_onyx == after);

    /* The charger: charger_apply() before the library load, so a boot on
     * the cable draws Charge Rate's budget (the default's 500 mA here: the
     * record is applied later, by settings_apply) through the load, not the
     * LTC4066's 100 mA reset state. */
    CHECK("boot: ...and every one of them with the charger asked for 500 mA",
          after > 0 && after_500 == after);

    /* The gauge: battery_refresh(1) before the loop's "as drawn" copy of it
     * (L->bat_glyph_prev). Primed after it, the first pass sees the gauge
     * "change" from the unprimed 0 % and paints the whole frame twice. */
    CHECK("boot: the first UI pass presents the main menu once",
          g_dropped == 0 && last_loading >= 0 && ui_frames == 1);

    to_main_menu();
    CHECK("boot: the main menu is drawn in Onyx",
          selected("Music") && hl_panel()[HL_W * (HL_H - 1)] == SURFACE_ONYX);
    power_cut("boot");
}

/* ---- the clock mark ------------------------------------------------------- */

/* The stamp `core sync` leaves: a day past the headless RTC's boot time, so
 * every boot that has not marked it acted on sets the clock (timesync rule 5
 * only makes a stamp inert once the RTC has run TIMESYNC_STALE_S past it,
 * and each session's RTC starts from hl_cfg.rtc_epoch again). */
#define STAMP_EPOCH     (1793534400u + 86400u)
#define STAMP_EPOCH_HEX "6AE87B40"

/* CORECFG.DAT's two slots, rewritten with a default record carrying the
 * stamp — the record config_encode() itself writes, so the firmware's
 * decoder takes it. Sequence numbers above the image's own. */
static int stamp_disk(const char *disk, const char *ranges)
{
    FILE *f = fopen(ranges, "r");
    if (!f) return -1;
    unsigned lba, cnt, cfg_lba = 0;
    char name[512];
    while (fscanf(f, "%u %u %511[^\n]", &lba, &cnt, name) == 3) {
        if (strcmp(name, "CORECFG.DAT") == 0) cfg_lba = lba;
    }
    fclose(f);
    if (cfg_lba == 0) return -1;

    static settings_t s;
    settings_defaults(&s);
    s.host_epoch   = STAMP_EPOCH;
    s.host_off_min = 0;
    int fd = open(disk, O_WRONLY);
    if (fd < 0) return -1;
    for (unsigned slot = 0; slot < CONFIG_SLOTS; slot++) {
        static uint16_t rec[CONFIG_SLOT_BYTES / 2];
        config_encode((uint8_t *)rec, &s, 0x10u + slot);
        off_t at = (off_t)(cfg_lba + slot * CONFIG_SLOT_SECTORS) * 512;
        if (pwrite(fd, rec, sizeof rec, at) != (ssize_t)sizeof rec) {
            close(fd);
            return -1;
        }
    }
    return close(fd);
}

/* The stamp is acted on, the forced write of the mark fails, and the mark is
 * re-armed after the boot's cfg_commit_clear() — so the idle path writes it.
 * Re-armed BEFORE the clear, the clear throws it away: nothing is pending,
 * nothing is written, and the next boot acts on the same stamp again. */
static _Noreturn void s_stamp(const char *disk, const char *ranges)
{
    T.suite = "boot-order(stamp)";
    hl_cfg.fail_writes = 1;                 /* the boot's first write: the mark */
    boot(disk, ranges);
    CHECK("stamp: the stamp is acted on (the clock is set)",
          log_has("core: timesync host " STAMP_EPOCH_HEX) &&
          log_has(" -> set rc 00000000"));
    CHECK("stamp: the forced write of the mark fails",
          hl_cfg.fail_writes == 0 && log_has("(retry pending)"));
    CHECK("stamp: ...and the mark is re-armed",
          log_has("core: timesync mark not saved yet"));
    log_wait_t w = { 0, "core: cfg save rc 00000000 seq " };
    int saved = hl_run_until(pred_log, &w, 250, 4000);
    if (!saved) {
        to_main_menu();
        saved = open_album() && hl_run_until(pred_log, &w, 250, 10000);
    }
    CHECK("stamp: the idle path writes the mark (cfg save rc 0)", saved);
    power_cut("stamp");
}

static _Noreturn void s_stamp_next(const char *disk, const char *ranges)
{
    T.suite = "boot-order(stamp-next)";
    boot(disk, ranges);
    /* The mark on the disk is what makes this boot's decision "none". (The
     * UART line prints the record AFTER the decision, so applied == host
     * there either way; the verdict is the evidence.) */
    CHECK("stamp-next: the same stamp is not acted on again (-> none)",
          log_has("core: timesync host " STAMP_EPOCH_HEX) &&
          log_has(" -> none rc ") && !log_has(" -> set rc "));
    power_cut("stamp-next");
}

typedef void (*session_fn)(const char *, const char *);

static int run(session_fn fn, const char *disk, const char *ranges,
               const char *name)
{
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) {
        fn(disk, ranges);
        _exit(3);
    }
    int st = 0;
    waitpid(p, &st, 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        fprintf(stderr, "boot-order: session %s FAILED\n", name);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    char disk[]  = "/tmp/boot_order_XXXXXX";
    char disk2[] = "/tmp/boot_order_stamp_XXXXXX";
    int fd  = mkstemp(disk);
    int fd2 = mkstemp(disk2);
    if (fd < 0 || fd2 < 0 || copy_disk(argv[1], disk) != 0 ||
        copy_disk(argv[1], disk2) != 0 || stamp_disk(disk2, argv[2]) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);
    close(fd2);

    /* Each session is its own process: the firmware's statics start from
     * .bss again, as on a real power-on, and each boots the disk the one
     * before it wrote. */
    int ok = run(s_save, disk, argv[2], "save");
    ok    &= run(s_boot, disk, argv[2], "boot");
    ok    &= run(s_stamp, disk2, argv[2], "stamp");
    ok    &= run(s_stamp_next, disk2, argv[2], "stamp-next");
    unlink(disk);
    unlink(disk2);
    return ok ? 0 : 1;
}
