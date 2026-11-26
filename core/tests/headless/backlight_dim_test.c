/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/backlight_dim_test.c — the idle backlight, judged by the
 * levels run_ui actually hands backlight_set().
 *
 * kernel/backlightpolicy.c decides WHEN an idle screen dims and goes out and
 * what the dimmed level is, and tests/kernel/backlightpolicy_test.c holds
 * that pure half. What it cannot hold is run_ui's call site: which setting
 * goes in as the timeout, which goes in as the brightness, and that the
 * answers reach the LED. Swapping an argument there (the dim level computed
 * from the TIMEOUT, say) passed every test, because nothing watched the
 * backlight after an idle. This does:
 *
 *   Settings > Display: Backlight 10 s (SELECT cycles 15 -> 30 -> 60 -> Off
 *   -> 5 -> 10), Brightness 22 (the slider, ten detents down from 32). Out
 *   to the main menu; the last press is the idle clock's zero. Then, from
 *   the HAL's backlight events, with nothing touched:
 *
 *     - the first change is to 22 / 4 = 5, the dimmed level, just after
 *       the 10 s timeout ("idle strictly longer than");
 *     - the next is to 0, just after timeout + 15 s;
 *     - nothing else in between, and nothing after;
 *   and one press re-lights at full brightness, 22.
 *
 * 22 is picked so that every plausible wrong argument gives another answer:
 * 10 / 4 = 2 (the timeout), 32 / 4 = 8 (the default brightness), 22 itself
 * (no dim). Each deadline is asserted to a window of one press plus a
 * couple of loop passes.
 *
 * Usage: backlight_dim_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "app/state.h"              /* g_settings, read while frozen */
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "backlight-dim", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

#define TIMEOUT_S   10
#define BRIGHT      22
#define DIM_LEVEL   (BRIGHT / 4)            /* 5; the floor of 1 is not hit */
#define OFF_AFTER_S 15                      /* BACKLIGHT_OFF_AFTER_DIM_US */
/* A deadline is "idle strictly longer than": the step lands on the first
 * main-loop pass past it. Allow the press itself plus a few idle passes. */
#define SLACK_US    300000u

/* ---- helpers (as settings_leave_test.c's) -------------------------------- */

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

/* The backlight events from index `from` on, into lv/t (at most max). */
static int bl_events(size_t from, uint32_t *lv, uint64_t *t, int max)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    int k = 0;
    for (size_t i = from; i < n && k < max; i++) {
        if (e[i].kind != HL_EV_BACKLIGHT) continue;
        lv[k] = e[i].arg;
        t[k]  = e[i].t_us;
        k++;
    }
    return k;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    char disk[] = "/tmp/backlight_dim_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);

    CHECK("image: the pre-allocated files' extents are known",
          load_ranges(argv[2]) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        unlink(disk);
        return 2;
    }

    /* Settings > Display. */
    CHECK("Settings > Display",
          select_row("Settings") && (tap(HL_BTN_SELECT), select_row("Display")));
    tap(HL_BTN_SELECT);
    CHECK("Backlight row", select_row("Backlight"));
    for (int i = 0; i < 5 && g_settings.backlight_secs != TIMEOUT_S; i++) {
        tap(HL_BTN_SELECT);                     /* cycles the timeout */
    }
    CHECK("Backlight: 10 s", g_settings.backlight_secs == TIMEOUT_S);
    CHECK("Brightness row", select_row("Brightness"));
    tap(HL_BTN_SELECT);                         /* edit the slider */
    for (int i = 0; i < 16 && g_settings.backlight_bright > BRIGHT; i++) {
        hl_scroll(-1);                          /* one detent */
        hl_run_ms(300);
    }
    tap(HL_BTN_SELECT);                         /* end the edit */
    CHECK("Brightness: 22", g_settings.backlight_bright == BRIGHT);
    CHECK("...and the panel is lit at it", hl_stats().backlight == BRIGHT);

    /* Out to the main menu. The last press restamps the idle clock: its
     * moment (between t_press and t_zero) is the zero of both deadlines. */
    tap(HL_BTN_MENU);
    tap(HL_BTN_MENU);
    uint64_t t_press = hl_stats().now_us;
    size_t ev0 = hl_events_mark();
    hl_press(HL_BTN_MENU, 60);
    hl_run_ms(100);
    uint64_t t_zero = hl_stats().now_us;
    CHECK("on the main menu, still lit at 22",
          selected("Settings") && hl_find_text(hl_panel(), "Music", NULL) &&
          hl_stats().backlight == BRIGHT);

    /* Idle through both deadlines, and a little past. */
    hl_run_ms((TIMEOUT_S + OFF_AFTER_S) * 1000u + 2000u);

    uint32_t lv[8];
    uint64_t t[8];
    int k = bl_events(ev0, lv, t, 8);
    for (int i = 0; i < k; i++) {
        printf("backlight %u at +%.3f s\n", lv[i],
               (double)(t[i] - t_press) / 1e6);
    }
    uint64_t dim_at = t_press + TIMEOUT_S * 1000000ull;
    uint64_t off_at = dim_at + OFF_AFTER_S * 1000000ull;
    CHECK("idle: exactly two backlight changes (dim, then off)", k == 2);
    CHECK("dim: to Brightness / 4 = 5", k >= 1 && lv[0] == DIM_LEVEL);
    CHECK("dim: just after the 10 s timeout",
          k >= 1 && t[0] > dim_at && t[0] <= dim_at + (t_zero - t_press) + SLACK_US);
    CHECK("off: to 0", k >= 2 && lv[1] == 0);
    CHECK("off: just after timeout + 15 s",
          k >= 2 && t[1] > off_at && t[1] <= off_at + (t_zero - t_press) + SLACK_US);

    /* One press re-lights at full brightness (from fully off it is swallowed:
     * it only lights the screen). */
    size_t ev1 = hl_events_mark();
    tap(HL_BTN_MENU);
    k = bl_events(ev1, lv, t, 8);
    CHECK("a press re-lights at full brightness, 22",
          k >= 1 && lv[0] == BRIGHT && hl_stats().backlight == BRIGHT);

    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    int rc = xfail_done(&T);
    fflush(NULL);
    unlink(disk);
    _exit(rc);
}
