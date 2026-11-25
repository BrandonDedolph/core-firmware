/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/drive_park_test.c — when the drive is let go.
 *
 * The real kernel/main.c on the headless disk:
 *
 *   1. browsing with nothing playing keeps the drive up, and the HOLD SWITCH
 *      parks it at once instead of after the 10 s idle timeout;
 *   2. playing and locked, the player parks between refill bursts with no
 *      browsing hold, and the music never underruns for it.
 *
 * Usage: drive_park_test DISK.img
 */

#include "headless.h"
#include "hl_screen.h"
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "drive-park", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

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
static int copy_disk(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_TRUNC);
    if (in < 0 || out < 0) return -1;
    char buf[1 << 16];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) return -1;
    }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s DISK.img\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/drive_park_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        unlink(disk);
        return 2;
    }

    /* ---- 1. not playing: the hold switch parks at once ------------------ */
    hl_tap(HL_BTN_SELECT, 500);                   /* Music */
    hl_tap(HL_BTN_SELECT, 800);                   /* Albums */
    CHECK("select Long Player", select_row("Long Player"));
    hl_tap(HL_BTN_SELECT, 800);                   /* its folder is read */
    CHECK("browsing: opening an album has the drive up", !hl_stats().ata_parked);
    hl_set_hold(1);
    hl_run_ms(500);
    CHECK("locked, nothing playing: the drive parks at once, not after 10 s",
          hl_stats().ata_parked);
    hl_set_hold(0);
    hl_run_ms(500);

    /* ---- 2. playing and locked: the player's own cadence --------------- */
    CHECK("select Long One", select_row("Long One"));
    uint32_t under0 = hl_stats().audio_underruns_total;
    hl_tap(HL_BTN_SELECT, 1000);
    CHECK("playing", hl_stats().audio_running);
    hl_set_hold(1);
    int parked = 0;
    for (int i = 0; i < 60 && !parked; i++) {
        hl_run_ms(500);
        parked = hl_stats().ata_parked;
    }
    CHECK("playing and locked: the drive parks between bursts (no browsing hold)",
          parked);
    hl_run_ms(20000);
    CHECK("...and the music never underruns for it",
          hl_stats().audio_underruns_total == under0 && hl_stats().audio_running);
    CHECK("no panic", hl_parked() == HL_RUNNING);

    unlink(disk);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}
