/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/settings_leave_test.c — leaving Settings commits the change.
 *
 * settings_leave() is the bookkeeping MENU does on the way out of the
 * Settings root (and the MENU hold, through scr_pop_to_root): it ends an
 * edit, captures the resume position, and commits the pending settings
 * change SOFT — now if the platters are turning, otherwise left pending for
 * the idle path or the next forced commit (kernel/cfg_commit.h). Without it a
 * change waits out the CFG_SAVE_DEBOUNCE_US (3 s) debounce, and a power cut
 * inside those 3 s loses it. firmware-walk never changes a setting, so
 * nothing else notices when that commit goes missing. Each session below is a
 * fresh process on the same disk image, and each ends with _exit() mid-run:
 * a hard power cut, no suspend, no forced commit.
 *
 * Session 1 (drive spinning, nothing playing):
 *   Settings > Playback > Repeat: Off -> All; MENU to the root; MENU out.
 *   "core: cfg save rc 0" is printed by THAT press — not before it (the
 *   debounce has not run out), and the drive was up so SOFT writes. Power is
 *   cut ~200 ms later, well under 3 s after the change.
 *
 * Session 2 (power on):
 *   Repeat reads All — the leave's write is what the boot loaded.
 *   Then: play Long One, lock until the player parks the drive between
 *   refill bursts, unlock, and change Repeat All -> One with the music
 *   playing and the drive PARKED. Leaving Settings now writes NOTHING and
 *   does not spin the drive up: SOFT never pays a 1-3 s spin-up for 1 KB
 *   while the user is waiting on the screen. Power cut ~200 ms later.
 *
 * Session 3 (power on):
 *   Repeat still reads All: the parked-drive change was pending, and a hard
 *   cut before the drive next turned lost it (the documented trade-off).
 *   Change it again, parked and playing, leave; then a folder read spins the
 *   drive up, and the idle path writes the pending change ("cfg save rc 0")
 *   as soon as the platters turn. Power cut.
 *
 * Session 4 (power on): Repeat reads One.
 *
 * Throughout: no write outside the pre-allocated files, no panic.
 *
 * Usage: settings_leave_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "settings-leave", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

#define DEBOUNCE_US 3000000u    /* kernel/cfg_commit.h CFG_SAVE_DEBOUNCE_US */

/* ---- helpers (as walk_test.c's) ------------------------------------------ */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int selected(const char *s)
{
    hl_text_hit_t h;
    return hl_find_text_in(hl_panel(), s, 40, HL_H, &h) && lum(h.ink) > lum(h.bg);
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

static int log_has(const char *s) { return strstr(hl_log(), s) != NULL; }
static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
}
/* `s` starts in [from, to) of the log. */
static int log_has_between(size_t from, size_t to, const char *s)
{
    const char *p = from <= hl_log_len() ? strstr(hl_log() + from, s) : NULL;
    return p && (size_t)(p - hl_log()) < to;
}
static int log_count_since(size_t from, const char *s)
{
    int n = 0;
    const char *p = from <= hl_log_len() ? hl_log() + from : NULL;
    while (p && (p = strstr(p, s)) != NULL) {
        n++;
        p++;
    }
    return n;
}
typedef struct { size_t from; const char *needle; } log_wait_t;
static int pred_log(void *a)
{
    const log_wait_t *w = a;
    return log_has_since(w->from, w->needle);
}
static long ev_find(size_t from, int kind)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    for (size_t i = from; i < n; i++) {
        if (e[i].kind == kind) return (long)i;
    }
    return -1;
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
    CHECK("boot: the settings record loaded and is writable",
          log_has("core: cfg load 00000001 writable 00000001 "));
}

/* A hard power cut: the firmware thread is frozen mid-wait and simply stops
 * existing. Nothing is suspended, nothing is forced out. */
static _Noreturn void power_cut(const char *what)
{
    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: power cut at fake time %.1f s, %u ATA writes\n", what,
           (double)s.now_us / 1e6, s.ata_writes);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}

static void to_main_menu(void)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
}

/* Main menu -> Music > Albums > Long Player: its folder is read, so a parked
 * drive spins up. */
static int open_album(void)
{
    int ok = select_row("Music");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Albums");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Long Player");
    hl_tap(HL_BTN_SELECT, 800);
    return ok;
}

/* Main menu -> Settings > Playback, the Repeat row selected. Reads no disk. */
static int to_repeat_row(void)
{
    int ok = select_row("Settings");
    tap(HL_BTN_SELECT);
    ok = ok && select_row("Playback");
    tap(HL_BTN_SELECT);
    return ok && select_row("Repeat");
}

/* The Repeat row's value reads `v`: `v` drawn on the Repeat row's line. */
static int repeat_reads(const char *v)
{
    hl_text_hit_t r;
    if (!hl_find_text_in(hl_panel(), "Repeat", 40, HL_H, &r)) return 0;
    return hl_find_text_in(hl_panel(), v, r.y - 8, r.y + r.h + 8, NULL);
}

/* Play Long One, then lock until the player parks the drive between refill
 * bursts, and unlock: the music plays on with the platters stopped. */
static int play_and_park(void)
{
    to_main_menu();
    int ok = open_album();
    ok = ok && select_row("Long One");
    hl_tap(HL_BTN_SELECT, 1000);
    ok = ok && hl_stats().audio_running;
    hl_set_hold(1);
    int parked = 0;
    for (int i = 0; i < 60 && !parked; i++) {
        hl_run_ms(500);
        parked = hl_stats().ata_parked;
    }
    hl_set_hold(0);
    hl_run_ms(500);
    return ok && parked;
}

/* The Repeat row selected: SELECT changes it, MENU back to the Settings root,
 * MENU out of Settings — that last press is settings_leave(). `to` is the
 * value the row should then read. Returns the
 * log offset just before the leaving press; *t_change is when the change was
 * made. */
static size_t change_and_leave(const char *to, uint64_t *t_change,
                               size_t *log_change)
{
    *log_change = hl_log_len();
    *t_change   = hl_stats().now_us;
    hl_tap(HL_BTN_SELECT, 300);                      /* the change           */
    CHECK("change: the Repeat row shows the new value", repeat_reads(to));
    hl_tap(HL_BTN_MENU, 300);                        /* Playback -> root     */
    size_t at_leave = hl_log_len();
    hl_tap(HL_BTN_MENU, 200);                        /* root -> out: leave   */
    return at_leave;
}

/* ---- session 1: drive spinning, the leave writes ------------------------- */

static _Noreturn void session_spinning(const char *disk, const char *ranges)
{
    T.suite = "settings-leave(1)";
    boot(disk, ranges);
    /* The boot's mount has long since been parked by the idle timeout (no
     * input since power-on); a folder read spins it up, and the browsing
     * that follows keeps the input fresh, so it stays up. */
    CHECK("s1: a folder read spins the drive up", open_album() && !hl_stats().ata_parked);
    to_main_menu();
    CHECK("s1: Settings > Playback > Repeat", to_repeat_row());
    CHECK("s1: Repeat starts Off", repeat_reads("Off"));
    CHECK("s1: the drive is up (browsing, input fresh)", !hl_stats().ata_parked);

    uint64_t t_change;
    size_t log_change;
    size_t at_leave = change_and_leave("All", &t_change, &log_change);
    uint64_t t_cut = hl_stats().now_us;
    CHECK("s1: the change is not written before the leave (debounce running)",
          !log_has_between(log_change, at_leave, "core: cfg save"));
    CHECK("s1: leaving Settings writes it at once (cfg save rc 0)",
          log_has_since(at_leave, "core: cfg save rc 00000000 seq "));
    CHECK("s1: back on the main menu", selected("Settings"));
    CHECK("s1: power is cut well inside the 3 s debounce",
          t_cut - t_change < DEBOUNCE_US / 2);
    power_cut("s1");
}

/* ---- session 2: playing, drive parked, the leave defers ------------------ */

static _Noreturn void session_parked_cut(const char *disk, const char *ranges)
{
    T.suite = "settings-leave(2)";
    boot(disk, ranges);
    to_main_menu();
    CHECK("s2: Settings > Playback > Repeat", to_repeat_row());
    CHECK("s2: Repeat reads All after the power cut (the leave's write)",
          repeat_reads("All"));

    CHECK("s2: playing Long One, the drive parked by the player", play_and_park());
    to_main_menu();
    CHECK("s2: Settings > Playback > Repeat", to_repeat_row());
    CHECK("s2: still playing, the drive still parked",
          hl_stats().audio_running && hl_stats().ata_parked);

    size_t ev0 = hl_events_mark();
    uint64_t t_change;
    size_t log_change;
    (void)change_and_leave("One", &t_change, &log_change);
    hl_run_ms(200);
    CHECK("s2: leaving Settings writes nothing while parked",
          !log_has_since(log_change, "core: cfg save") &&
          ev_find(ev0, HL_EV_ATA_WRITE) < 0);
    CHECK("s2: ...and does not spin the drive up for it",
          ev_find(ev0, HL_EV_ATA_SPINUP) < 0 && ev_find(ev0, HL_EV_ATA_WAKEUP) < 0 &&
          hl_stats().ata_parked);
    CHECK("s2: the music plays on", hl_stats().audio_running);
    CHECK("s2: power is cut inside the debounce",
          hl_stats().now_us - t_change < DEBOUNCE_US);
    power_cut("s2");
}

/* ---- session 3: the pending change lands at the next spin-up ------------- */

static _Noreturn void session_parked_spin(const char *disk, const char *ranges)
{
    T.suite = "settings-leave(3)";
    boot(disk, ranges);
    to_main_menu();
    CHECK("s3: Settings > Playback > Repeat", to_repeat_row());
    CHECK("s3: Repeat still reads All: the parked change died with the power",
          repeat_reads("All"));

    CHECK("s3: playing Long One, the drive parked by the player", play_and_park());
    to_main_menu();
    CHECK("s3: Settings > Playback > Repeat", to_repeat_row());
    CHECK("s3: still playing, the drive still parked",
          hl_stats().audio_running && hl_stats().ata_parked);
    uint64_t t_change;
    size_t log_change;
    (void)change_and_leave("One", &t_change, &log_change);
    hl_run_ms(200);
    CHECK("s3: leaving parked writes nothing",
          !log_has_since(log_change, "core: cfg save") && hl_stats().ata_parked);

    /* A folder read spins the platters; the idle path writes the pending
     * change as soon as they turn (the debounce has run out by then). */
    size_t log1 = hl_log_len();
    CHECK("s3: Music > Albums > Long Player (a folder read)", open_album());
    log_wait_t w = { log1, "core: cfg save rc 00000000 seq " };
    CHECK("s3: the pending change lands once the drive turns",
          hl_run_until(pred_log, &w, 100, 6000));
    CHECK("s3: ...once", log_count_since(log1, "core: cfg save rc") == 1);
    power_cut("s3");
}

/* ---- session 4: the change that landed is the one booted ----------------- */

static _Noreturn void session_verify(const char *disk, const char *ranges)
{
    T.suite = "settings-leave(4)";
    boot(disk, ranges);
    to_main_menu();
    CHECK("s4: Settings > Playback > Repeat", to_repeat_row());
    CHECK("s4: Repeat reads One", repeat_reads("One"));
    power_cut("s4");
}

static int run_session(void (*fn)(const char *, const char *),
                       const char *disk, const char *ranges, const char *name)
{
    int st = 0;
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) fn(disk, ranges);
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok) fprintf(stderr, "settings-leave: session %s FAILED\n", name);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/settings_leave_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);

    int ok = run_session(session_spinning,    disk, argv[2], "1 (spinning)");
    ok    &= run_session(session_parked_cut,  disk, argv[2], "2 (parked, cut)");
    ok    &= run_session(session_parked_spin, disk, argv[2], "3 (parked, spin-up)");
    ok    &= run_session(session_verify,      disk, argv[2], "4 (verify)");
    unlink(disk);
    return ok ? 0 : 1;
}
