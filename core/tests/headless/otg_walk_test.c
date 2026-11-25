/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/otg_walk_test.c — On-The-Go, end to end, through the UI.
 *
 * The otg / otg-slot / otg-store unit tests link only the list, the slot
 * writer and the COREOTG.DAT store. What joins them to the library — turning a
 * song into a locator pair, binding pairs back to songs, building Save's row
 * array from the bound songs, the screen's remove — is app-level code they
 * never run. This drives that code on the headless disk, by the gestures a
 * user makes, and judges it by what the screen and the UART say:
 *
 * Session 1:
 *   1. add       hold Select on the "Beethoven Bits" ALBUM row (three songs,
 *                in tracklist order), then on the "Long Two" TRACK row of
 *                another album; the confirmation banners say so
 *   2. list      Playlists > On-The-Go shows the four, in that order
 *   3. remove    hold Select on "Andante": the other three stay, in order
 *   4. save      Save Playlist: "otg slot save rc 0 wrote 3 of 3", the live
 *                list empties, Playlists lists "On-The-Go 1"
 *   5. open      the slot opens to exactly Allegro, Sine, Long Two, in that
 *                order, every row answered by the index (so every line names
 *                the right FILE — a row whose path is wrong is either dropped
 *                by the writer or not bound), and the walk agrees
 *   6. delete    Delete Playlist (twice): "otg slot erase rc 0", the slot is
 *                gone from Playlists
 *   7. rebuild   a new live list, Mono Mp3 then Long One, committed to
 *                COREOTG.DAT ("otg save rc 0")
 *
 * Session 2 (a fresh process: a true power-on of the disk session 1 wrote):
 *   8. reboot    "otg load 2", and On-The-Go shows Mono Mp3 then Long One —
 *                bound to the songs, not "Not on this iPod", nothing MISSING
 *
 * Throughout: no write outside the pre-allocated files, no panic.
 *
 * Usage: otg_walk_test DISK.img DISK.img.ranges [SHOT_DIR]
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

static xfail_ctx T = { "otg-walk", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

static const char *g_shots;
static int g_shot_n;

/* ---- helpers (as walk_test.c's) ------------------------------------------ */

static void shot(const char *name)
{
    if (!g_shots) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/otg_walk_%02d_%s.bmp", g_shots, g_shot_n++, name);
    (void)hl_write_bmp(path);
}

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int on_screen(const char *s) { return hl_find_text(hl_panel(), s, NULL); }
/* Below the header and status strip only: the strip carries the playing
 * track's name and the banner sits over the top chrome. */
static int in_list(const char *s) { return hl_find_text_in(hl_panel(), s, 40, HL_H, NULL); }
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

/* Hold Select past SEL_HOLD_US (450 ms) — the add / remove gesture. Returns
 * with the confirmation banner (900 ms) still up. */
static void hold_select(void)
{
    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(800);
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

/* hl_run_until predicate: the header reads `a`. */
static int pred_header(void *a)
{
    return hl_find_text_in(hl_panel(), (const char *)a, 0, 40, NULL);
}

/* The list rows, top to bottom, are exactly `want[0..n)`: each is on the
 * panel below the header, and each one's ink box is lower than the last.
 * The rows are two-line (title + artist) at ROW_H2 = 32 px. */
static int rows_in_order(const char *const *want, int n)
{
    int last_y = -1;
    for (int i = 0; i < n; i++) {
        hl_text_hit_t h;
        if (!hl_find_text_in(hl_panel(), want[i], 40, HL_H, &h)) return 0;
        if (h.y <= last_y + 16) return 0;
        last_y = h.y;
    }
    return 1;
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

static _Noreturn void session_end(const char *what)
{
    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: fake time %.1f s, %u ATA writes\n", what,
           (double)s.now_us / 1e6, s.ata_writes);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);                     /* the firmware thread is frozen mid-wait */
}

/* Main menu -> Music -> Playlists, whatever screen we are on. */
static void to_playlists(void)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    CHECK("nav: main menu", select_row("Music"));
    tap(HL_BTN_SELECT);
    CHECK("nav: Music > Playlists", select_row("Playlists"));
    tap(HL_BTN_SELECT);
    /* The folder is read on entry; a parked drive spins up first. */
    CHECK("nav: the Playlists screen", hl_run_until(pred_header, "Playlists", 250, 5000));
}

/* Main menu -> Music -> Albums -> `album`, entered. */
static void to_album(const char *album)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
    CHECK("nav: main menu", select_row("Music"));
    tap(HL_BTN_SELECT);
    CHECK("nav: Music > Albums", select_row("Albums"));
    tap(HL_BTN_SELECT);
    CHECK("nav: the album row", select_row(album));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);               /* the tracklist is read; maybe a spin-up */
}

/* Playlists (On-The-Go row selected) -> the On-The-Go screen. Opening it
 * probes the five slot files for a free one, so it may wait on the drive. */
static void open_otg(void)
{
    tap(HL_BTN_SELECT);
    CHECK("nav: the On-The-Go screen", hl_run_until(pred_header, "On-The-Go", 250, 5000));
}

/* ---- session 1 ----------------------------------------------------------- */

static _Noreturn void session_build(const char *disk, const char *ranges)
{
    CHECK("image: the pre-allocated files' extents are known", load_ranges(ranges) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        _exit(2);
    }
    CHECK("boot: reached the browser", log_has("core: entering browser"));
    CHECK("boot: the live list starts empty and writable",
          log_has("core: otg load 0 writable 00000001 "));

    /* ---- 1. add: an album row, then one track of another album ---------- */
    tap(HL_BTN_SELECT);                               /* main menu -> Music */
    CHECK("add: Music > Albums", select_row("Albums"));
    tap(HL_BTN_SELECT);
    CHECK("add: the Beethoven Bits album row", select_row("Beethoven Bits"));
    hold_select();
    shot("add_album");
    CHECK("add: holding Select on an album adds its three songs (banner)",
          on_screen("Added 3 songs"));
    CHECK("add: ...and does not enter the album",
          in_list("Long Player") && !in_list("Allegro"));
    hl_run_ms(1000);

    CHECK("add: enter Long Player", select_row("Long Player"));
    tap(HL_BTN_SELECT);
    CHECK("add: the Long Two track row", select_row("Long Two"));
    hold_select();
    shot("add_track");
    CHECK("add: holding Select on a track adds it (banner)",
          on_screen("Added to On-The-Go"));
    CHECK("add: ...and does not play it", !hl_stats().audio_running &&
          !on_screen("Now Playing"));
    hl_run_ms(1000);

    /* ---- 2. the On-The-Go screen ---------------------------------------- */
    to_playlists();
    shot("playlists");
    CHECK("list: On-The-Go is pinned first, selected", selected("On-The-Go"));
    CHECK("list: ...with its count", in_list("4"));
    open_otg();
    shot("otg");
    CHECK("list: the On-The-Go screen (header)",
          hl_find_text_in(hl_panel(), "On-The-Go", 0, 40, NULL));
    CHECK("list: the action rows", in_list("Clear Playlist") && in_list("Save Playlist"));
    {
        const char *want[] = { "Save Playlist", "Allegro", "Andante", "Sine", "Long Two" };
        CHECK("list: the album in tracklist order, then the track",
              rows_in_order(want, 5));
    }
    CHECK("list: every entry bound to a song", !in_list("Not on this iPod"));

    /* ---- 3. remove ------------------------------------------------------ */
    CHECK("remove: select Andante", select_row("Andante"));
    hold_select();
    shot("removed");
    CHECK("remove: banner", on_screen("Removed"));
    hl_run_ms(1000);
    CHECK("remove: Andante is gone", !in_list("Andante"));
    {
        const char *want[] = { "Save Playlist", "Allegro", "Sine", "Long Two" };
        CHECK("remove: the other three stay, in order", rows_in_order(want, 4));
    }

    /* ---- 4. save -------------------------------------------------------- */
    CHECK("save: select Save Playlist", select_row("Save Playlist"));
    size_t mark = hl_log_len();
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    shot("saved");
    CHECK("save: into the first free slot",
          log_has_since(mark, "core: otg slot 1 lba "));
    CHECK("save: every row written",
          log_has_since(mark, "core: otg slot save rc 00000000 wrote 3 of 3"));
    CHECK("save: the emptied live list is committed",
          log_has_since(mark, "core: otg save rc 00000000 seq "));
    CHECK("save: back on Playlists, the saved list selected", selected("On-The-Go 1"));
    CHECK("save: the live list is empty now (no count on its row)", !in_list("3"));

    /* ---- 5. open the saved slot ----------------------------------------- */
    mark = hl_log_len();
    tap(HL_BTN_SELECT);
    hl_run_ms(500);
    shot("slot");
    CHECK("open: the slot's tracklist (header)",
          hl_find_text_in(hl_panel(), "On-The-Go 1", 0, 40, NULL));
    CHECK("open: three rows, all three answered by the index",
          log_has_since(mark, "core: playlist rows 00000003 index 00000003"));
    CHECK("open: the index's rows are the walk's",
          log_has_since(mark, "core: playlist index check mismatch 00000000"));
    {
        const char *want[] = { "Allegro", "Sine", "Long Two", "Delete Playlist" };
        CHECK("open: the saved tracks, in the saved order, then Delete",
              rows_in_order(want, 4));
    }
    CHECK("open: the removed track was not saved", !in_list("Andante"));

    /* ---- 6. delete the slot --------------------------------------------- */
    CHECK("delete: select Delete Playlist", select_row("Delete Playlist"));
    tap(HL_BTN_SELECT);
    CHECK("delete: the row asks first", in_list("Delete? Select again"));
    mark = hl_log_len();
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    shot("deleted");
    CHECK("delete: the slot is erased",
          log_has_since(mark, "core: otg slot erase rc 00000000"));
    CHECK("delete: back on Playlists, the slot gone",
          hl_find_text_in(hl_panel(), "Playlists", 0, 40, NULL) &&
          in_list("Mixed") && !in_list("On-The-Go 1"));

    /* ---- 7. a new live list, committed ---------------------------------- */
    to_album("Mono Mix");
    CHECK("rebuild: Mono Mp3", select_row("Mono Mp3"));
    hold_select();
    hl_run_ms(1000);
    to_album("Long Player");
    CHECK("rebuild: Long One", select_row("Long One"));
    mark = hl_log_len();
    hold_select();
    log_wait_t w = { mark, "core: otg save rc 00000000 seq " };
    CHECK("rebuild: the live list is committed to COREOTG.DAT",
          hl_run_until(pred_log, &w, 500, 60000));
    to_playlists();
    open_otg();
    shot("rebuilt");
    {
        const char *want[] = { "Save Playlist", "Mono Mp3", "Long One" };
        CHECK("rebuild: On-The-Go shows the two, in order", rows_in_order(want, 3));
    }
    session_end("build");
}

/* ---- session 2 ----------------------------------------------------------- */

static _Noreturn void session_reboot(const char *disk)
{
    T.suite = "otg-walk(2)";
    g_shot_n = 50;
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        _exit(2);
    }
    CHECK("reboot: reached the browser", log_has("core: entering browser"));
    CHECK("reboot: the live list comes back from COREOTG.DAT",
          log_has("core: otg load 2 writable 00000001 "));
    to_playlists();
    shot("reboot_playlists");
    CHECK("reboot: On-The-Go row with its count", selected("On-The-Go") && in_list("2"));
    open_otg();
    shot("reboot_otg");
    {
        const char *want[] = { "Save Playlist", "Mono Mp3", "Long One" };
        CHECK("reboot: bound to the same songs, in order", rows_in_order(want, 3));
    }
    CHECK("reboot: no entry unbound", !in_list("Not on this iPod"));
    CHECK("reboot: nothing missing in the header", !on_screen("MISSING"));
    session_end("reboot");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges [SHOT_DIR]\n", argv[0]);
        return 2;
    }
    g_shots = argc > 3 ? argv[3] : NULL;
    char disk[] = "/tmp/otg_walk_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);

    int st1 = 0, st2 = 0;
    fflush(NULL);
    pid_t p1 = fork();
    if (p1 == 0) session_build(disk, argv[2]);
    waitpid(p1, &st1, 0);
    fflush(NULL);
    pid_t p2 = fork();
    if (p2 == 0) session_reboot(disk);
    waitpid(p2, &st2, 0);
    unlink(disk);

    int ok1 = WIFEXITED(st1) && WEXITSTATUS(st1) == 0;
    int ok2 = WIFEXITED(st2) && WEXITSTATUS(st2) == 0;
    if (!ok1) fprintf(stderr, "otg-walk: session 1 (build) FAILED\n");
    if (!ok2) fprintf(stderr, "otg-walk: session 2 (reboot) FAILED\n");
    return ok1 && ok2 ? 0 : 1;
}
