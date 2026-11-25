/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/playlist_index_test.c — opening a playlist answers indexed
 * tracks from RAM, and the answer is the disk walk's.
 *
 * The real kernel/main.c, booted on the disk tests/scripts/make_headless_disk.py
 * builds, opens Music > Playlists > Mixed: every entry shape the lookup has to
 * get right (relative, absolute, case-changed, backslashed), a file on the
 * disk the index does not know, and a missing one. The headless build of
 * playlist_open (MMIO_MOCK) resolves the playlist a second time by walking
 * every entry and logs whether the two agree row for row
 * (pl_index_selfcheck); the hw image only logs how many rows the index
 * answered.
 *
 * Usage: playlist_index_test DISK.img
 */

#include "headless.h"
#include "hl_screen.h"
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "playlist-index", 0, 0, 0 };
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
static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
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
    char disk[] = "/tmp/playlist_index_XXXXXX";
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
    CHECK("boot: reached the browser", strstr(hl_log(), "core: entering browser") != NULL);

    hl_tap(HL_BTN_SELECT, 500);                   /* main menu -> Music */
    CHECK("Music: Playlists row", select_row("Playlists"));
    hl_tap(HL_BTN_SELECT, 800);
    CHECK("Playlists: Mixed is listed", select_row("Mixed"));

    size_t mark = hl_log_len();
    hl_tap(HL_BTN_SELECT, 800);
    CHECK("open: five playable rows, four answered by the index "
          "(the unindexed file is walked, the missing one is absent)",
          log_has_since(mark, "core: playlist rows 00000005 index 00000004"));
    CHECK("open: the index's rows are the walk's, row for row",
          log_has_since(mark, "core: playlist index check mismatch 00000000"));
    CHECK("open: the unindexed file is a row (walked)",
          hl_find_text_in(hl_panel(), "Bonus", 40, HL_H, NULL));
    CHECK("no panic", hl_parked() == HL_RUNNING);

    unlink(disk);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);                     /* the firmware thread is parked mid-wait */
}
