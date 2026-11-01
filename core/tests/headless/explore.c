/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/explore.c — drive the headless firmware by hand.
 *
 * Not a test: a command-line driver for looking at what the real firmware
 * draws, writing screenshots, and working out the steps a new assertion in
 * walk_test.c should take. Boots DISK (it is WRITTEN to: pass a copy), runs
 * 12 s of fake time, then performs each command in order:
 *
 *   run:MS        let the firmware run MS milliseconds of fake time
 *   sel menu play right left
 *                 tap that button (60 ms) and run 400 ms
 *   hold:BTN:MS   hold sel|menu|play|right|left for MS, then run 400 ms
 *   down:N up:N   N wheel rows (150 ms apart), then settle
 *   holdsw:0|1    the Hold switch
 *   jack:0|1      headphones out / in
 *   shot:PATH     write the panel as a 24-bit BMP
 *   find:TEXT     report whether TEXT is on the panel, where, in what colours
 *   stats         one line of hl_stats()
 *   log           the UART output since the last `log`
 *   evs           the hardware timeline (rect presents omitted)
 *
 *   ./build-sim/tests/headless_explore copy.img sel sel shot:albums.bmp
 */

#include "headless.h"
#include "hl_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned button(const char *s)
{
    if (!strncmp(s, "sel", 3))   return HL_BTN_SELECT;
    if (!strncmp(s, "menu", 4))  return HL_BTN_MENU;
    if (!strncmp(s, "play", 4))  return HL_BTN_PLAY;
    if (!strncmp(s, "right", 5)) return HL_BTN_RIGHT;
    if (!strncmp(s, "left", 4))  return HL_BTN_LEFT;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s DISK.img [commands...]\n", argv[0]);
        return 2;
    }
    if (hl_boot(argv[1], 12000) != 0) return 1;
    size_t mark = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        unsigned b;
        if (!strncmp(a, "run:", 4)) {
            hl_run_ms((uint32_t)atoi(a + 4));
        } else if (!strncmp(a, "hold:", 5) && (b = button(a + 5)) != 0) {
            const char *ms = strchr(a + 5, ':');
            uint32_t t = ms ? (uint32_t)atoi(ms + 1) : 1000u;
            hl_press(b, t);
            hl_run_ms(t + 400);
        } else if ((b = button(a)) != 0) {
            hl_tap(b, 400);
        } else if (!strncmp(a, "down:", 5) || !strncmp(a, "up:", 3)) {
            int n = atoi(strchr(a, ':') + 1);
            hl_scroll(a[0] == 'd' ? n : -n);
            hl_run_ms((uint32_t)(n * 150 + 400));
        } else if (!strncmp(a, "holdsw:", 7)) {
            hl_set_hold(atoi(a + 7));
            hl_run_ms(400);
        } else if (!strncmp(a, "jack:", 5)) {
            hl_set_headphones(atoi(a + 5));
            hl_run_ms(400);
        } else if (!strncmp(a, "shot:", 5)) {
            if (hl_write_bmp(a + 5) != 0) fprintf(stderr, "cannot write %s\n", a + 5);
        } else if (!strncmp(a, "find:", 5)) {
            hl_text_hit_t h;
            int f = hl_find_text(hl_panel(), a + 5, &h);
            if (f) {
                printf("find '%s': at %d,%d %dx%d face %d ink %04X bg %04X\n",
                       a + 5, h.x, h.y, h.w, h.h, h.font, h.ink, h.bg);
            } else {
                printf("find '%s': not on screen\n", a + 5);
            }
        } else if (!strcmp(a, "stats")) {
            hl_stats_t s = hl_stats();
            printf("t=%.3f presents full %u rect %u refused %u | bl %d lcd_slept %d"
                   " | ata r %u w %u spinups %u parked %d bad %u | audio open %d"
                   " run %d cold %d %u Hz played %llu pulled %llu nonzero %llu"
                   " underruns %u | boost %d | parked %d\n",
                   (double)s.now_us / 1e6, s.presents_full, s.presents_rect,
                   s.presents_refused, s.backlight, s.lcd_slept, s.ata_reads,
                   s.ata_writes, s.ata_spinups, s.ata_parked, s.bad_writes,
                   s.audio_open, s.audio_running, s.audio_cold, s.audio_rate,
                   (unsigned long long)s.audio_frames_played,
                   (unsigned long long)s.audio_frames_pulled,
                   (unsigned long long)s.audio_nonzero, s.audio_underruns,
                   s.cpu_boost, (int)hl_parked());
        } else if (!strcmp(a, "log")) {
            fputs(hl_log() + mark, stdout);
            mark = hl_log_len();
        } else if (!strcmp(a, "evs")) {
            size_t n;
            const hl_ev_t *e = hl_events(&n);
            for (size_t k = 0; k < n; k++) {
                if (e[k].kind != HL_EV_PRESENT_RECT) {
                    printf("  %10.3f  ev %2d  arg %u\n", (double)e[k].t_us / 1e6,
                           e[k].kind, e[k].arg);
                }
            }
        } else {
            fprintf(stderr, "unknown command: %s\n", a);
            return 2;
        }
    }
    return 0;
}
