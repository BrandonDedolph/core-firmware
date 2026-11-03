/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/walk_test.c — firmware-walk: the real kernel/main.c, from a
 * cold boot to a PMU power-off, on the host.
 *
 * Two sessions over the disk tests/scripts/make_headless_disk.py builds
 * (three albums: the codec vectors, two 90 s generated FLACs — one ~16 MB
 * uncompressed, twice the anti-skip buffer — and a 22 kHz mono pair), in
 * deterministic fake time (hal/headless/headless.h), each in its own process
 * so the second is a true power-on of the disk the first one wrote.
 *
 * Session 1, the walk:
 *
 *   1. boot          mount, config/evlog/OTG mounted at the LBAs the host
 *                    tools computed, main menu on the panel, lit
 *   2. browse        Music > Albums (all three, from CORELIB.IDX)
 *   2b. codecs       an MP3 plays (44.1 kHz), a 22 kHz mono MP3 re-opens the
 *                    DAC at 22050 Hz, both decode to real PCM
 *   3. play          Long Player's tracklist; SELECT on "Long One": Now
 *                    Playing, the DAC opened at
 *                    44.1 kHz and fed real decoded PCM, the clock moving
 *   4. bars          the tracklist's now-playing bars MOVE while playing,
 *                    and while paused (v0.2.2, bars_clock in main.c) they do
 *                    not move, are not re-pushed, and a full repaint redraws
 *                    the SAME frozen shape; they move again on resume
 *   5. settings      Settings > About / Battery / Boot Details render
 *                    (ui/screen_settings.c) with the library's real counts
 *   6. idle sleep    paused + untouched: the codec powers down, the drive
 *                    parks, the backlight times out, and at 120 s
 *                    (kernel/idlesleep.c) the device suspends — white frame,
 *                    LED off, drive parked, boost released (the 2026-09-24
 *                    idle-drain bug was the absence of exactly this)
 *   7. wake          a key brings it back in the safe order (drive up, the
 *                    current screen painted and PRESENTED before the LED is
 *                    lit — the 2026-09-13 dark/white-screen class), the wake
 *                    press is swallowed, the paused track stays paused
 *   8. escalation    left suspended on battery, it escalates to a PMU
 *                    standby after 30 minutes and powers off
 *
 * Session 2, the reboot: the settings record and event log session 1 wrote
 * are the ones loaded (FINAL flag seen), the track comes back paused at the
 * second it was paused, the idle drive parks under it, and PLAY then plays
 * 40 s through refill bursts and a spin-up without one underrun (the
 * 2026-09-13 resume stall, player.c d9197e4: with that fix reverted this
 * session fails with 5 underruns).
 *
 * Throughout: no write ever lands outside the pre-allocated files (the
 * image's .ranges file), no write breaks the drive's 1 KB physical-sector
 * rule, and the firmware never panics.
 *
 * Usage: firmware_walk_test DISK.img DISK.img.ranges [SHOT_DIR]
 * The image is copied first; the firmware writes to the copy. With SHOT_DIR,
 * each step's panel is saved there as firmware_walk_NN_<step>.bmp.
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

static xfail_ctx T = { "firmware-walk", 0, 0, 0 };
static const char *g_shots;
static int g_shot_n;

#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* ---- small helpers -------------------------------------------------------- */

static void shot(const char *name)
{
    if (!g_shots) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/firmware_walk_%02d_%s.bmp", g_shots,
             g_shot_n++, name);
    (void)hl_write_bmp(path);
}

static int on_screen(const char *s)
{
    return hl_find_text(hl_panel(), s, NULL);
}

/* Light ink on a dark ground: the selection bar. Looked for below the header
 * (y >= 40), so the status strip's copy of the playing track's name — top
 * left, small, grey — is never the one judged. */
static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int selected(const char *s)
{
    hl_text_hit_t h;
    return hl_find_text_in(hl_panel(), s, 40, HL_H, &h) && lum(h.ink) > lum(h.bg);
}

static int log_has(const char *s) { return strstr(hl_log(), s) != NULL; }
static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
}

/* hl_run_until predicate: `needle` printed at or after log offset `from`. */
typedef struct { size_t from; const char *needle; } log_wait_t;
static int pred_log(void *a)
{
    const log_wait_t *w = a;
    return log_has_since(w->from, w->needle);
}

/* Move the selection onto the row reading `s`: to the top of the list, then
 * down a row at a time (a pace the wheel's acceleration curve ignores). */
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

static int all_white(void)
{
    return hl_count_color(hl_panel(), 0xFFFF, 0, 0, HL_W, HL_H) == HL_W * HL_H;
}

/* Index of the first event of `kind` at or after `from` (and arg >= min_arg),
 * or -1. */
static long ev_find(size_t from, int kind, uint32_t min_arg)
{
    size_t n;
    const hl_ev_t *e = hl_events(&n);
    for (size_t i = from; i < n; i++) {
        if (e[i].kind == kind && e[i].arg >= min_arg) return (long)i;
    }
    return -1;
}
static int ev_count(size_t from, int kind, uint32_t arg, int match_arg)
{
    size_t n;
    int c = 0;
    const hl_ev_t *e = hl_events(&n);
    for (size_t i = from; i < n; i++) {
        if (e[i].kind == kind && (!match_arg || e[i].arg == arg)) c++;
    }
    return c;
}

/* The tracklist's now-playing glyph on row 0 of the album detail: main.c
 * paints it at (15, DET_LIST_Y0 + 6) in a 9x9 box and partial-presents the
 * box one pixel wider. */
#define BARS_X 15
#define BARS_Y (108 + 6)
#define BARS_W 10
#define BARS_H 9
#define BARS_RECT_ARG (((uint32_t)BARS_X << 16) | (uint32_t)BARS_Y)

/* How many distinct bar shapes appear over `n` samples `step_ms` apart. */
static int bar_shapes(int n, uint32_t step_ms)
{
    uint16_t seen[32][BARS_W * BARS_H];
    int ns = 0;
    for (int i = 0; i < n && i < 32; i++) {
        uint16_t cur[BARS_W * BARS_H];
        hl_copy_rect(cur, hl_panel(), BARS_X, BARS_Y, BARS_W, BARS_H);
        int dup = 0;
        for (int k = 0; k < ns; k++) {
            if (memcmp(seen[k], cur, sizeof cur) == 0) { dup = 1; break; }
        }
        if (!dup) memcpy(seen[ns++], cur, sizeof cur);
        hl_run_ms(step_ms);
    }
    return ns;
}

/* The Now Playing transport band: elapsed / remaining and the progress bar. */
#define BAND_Y 180
#define BAND_H 40

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

/* First LBA of each pre-allocated file, as the HOST computed it — the number
 * every write path's first-flash procedure compares the boot line against. */
static unsigned g_cfg_lba, g_log_lba, g_otg_lba;

static int load_ranges(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned lba, cnt;
    char name[512];
    int n = 0;
    while (fscanf(f, "%u %u %511[^\n]", &lba, &cnt, name) == 3) {
        if (hl_allow_writes(lba, cnt) == 0) n++;
        if (!strcmp(name, "CORECFG.DAT")) g_cfg_lba = lba;
        if (!strcmp(name, "CORELOG.BIN")) g_log_lba = lba;
        if (!strcmp(name, "COREOTG.DAT")) g_otg_lba = lba;
    }
    fclose(f);
    return n;
}

static int log_hasf(const char *fmt, unsigned a, unsigned b)
{
    char want[160];
    snprintf(want, sizeof want, fmt, a, b);
    return log_has(want);
}

/* Now Playing's elapsed clock ("M:SS", left half of the transport band), in
 * seconds, or -1 when none is drawn. */
static int np_elapsed_s(void)
{
    char t[16];
    hl_text_hit_t h;
    for (int sec = 0; sec < 600; sec++) {
        snprintf(t, sizeof t, "%d:%02d", sec / 60, sec % 60);
        if (hl_find_text_in(hl_panel(), t, BAND_Y, BAND_Y + BAND_H, &h) &&
            h.x < HL_W / 2) {
            return sec;
        }
    }
    return -1;
}

static int g_report_fd = -1;

static _Noreturn void session_end(const char *what)
{
    hl_stats_t s = hl_stats();
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: fake time %.1f s, %u presents, %u ATA writes\n", what,
           (double)s.now_us / 1e6, s.presents_full + s.presents_rect, s.ata_writes);
    int rc = xfail_done(&T);
    fflush(NULL);
    /* The firmware thread is parked or frozen mid-wait; leave without
     * unwinding it. */
    _exit(rc);
}

/* ---- session 1: the walk -------------------------------------------------- */

static _Noreturn void session_walk(const char *disk, const char *ranges)
{
    int nranges = load_ranges(ranges);
    CHECK("image: the pre-allocated files' extents are known", nranges >= 3);

    /* ---- 1. boot ---------------------------------------------------------- */
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        _exit(2);
    }
    shot("boot");
    CHECK("boot: firmware running (no panic, no idle fallback)",
          hl_parked() == HL_RUNNING);
    CHECK("boot: FAT32 mounted from the MBR partition", log_has("core: mount rc 00000000"));
    CHECK("boot: reached the browser", log_has("core: entering browser"));
    CHECK("image: CORECFG.DAT, CORELOG.BIN, COREOTG.DAT located",
          g_cfg_lba && g_log_lba && g_otg_lba);
    /* Slot 1 is one physical sector (2 LBAs) past slot 0 (kernel/config.c). */
    CHECK("boot: CORECFG.DAT loaded, writable, at the LBAs the host computed",
          log_hasf("core: cfg load 00000001 writable 00000001 seq 00000001 lba %08X/%08X",
                   g_cfg_lba, g_cfg_lba + 2));
    CHECK("boot: event log mounted, header at the LBA the host computed",
          log_hasf("core: evlog on seq 00000000 boot 00000001 prev none lba %08X/%08X",
                   g_log_lba, g_log_lba + 4));
    CHECK("boot: OTG store mounted writable, slot 0 at the LBA the host computed",
          log_hasf("core: otg load 0 writable 00000001 seq 00000001 lba %08X/%08X",
                   g_otg_lba, g_otg_lba + 10));
    CHECK("boot: RTC read through the PMU (valid)", log_has(" valid 00000001 epoch 6AE729C0"));
    CHECK("boot: main menu shows Music, selected", selected("Music"));
    CHECK("boot: main menu shows Settings", on_screen("Settings"));
    hl_stats_t s = hl_stats();
    CHECK("boot: backlight lit", s.backlight == 32);
    CHECK("boot: panel awake", !s.lcd_slept);
    CHECK("boot: codec held cold until the first track", s.audio_cold && !s.audio_open);

    /* ---- 2. browse -------------------------------------------------------- */
    tap(HL_BTN_SELECT);
    shot("music");
    CHECK("browse: Music menu (header)", hl_find_text_in(hl_panel(), "Music", 0, 40, NULL));
    CHECK("browse: Albums is the default row", selected("Albums"));
    tap(HL_BTN_SELECT);
    shot("albums");
    CHECK("browse: Albums list (header)", hl_find_text_in(hl_panel(), "Albums", 0, 40, NULL));
    CHECK("browse: album 1 from CORELIB.IDX", on_screen("Beethoven Bits"));
    CHECK("browse: album 2 from CORELIB.IDX", on_screen("Long Player"));
    CHECK("browse: album 3 from CORELIB.IDX", on_screen("Mono Mix"));
    CHECK("browse: album artist subtitle", on_screen("Test Tones"));

    /* ---- 2b. the other decoders: an MP3, then a 22 kHz mono MP3 ----------- */
    CHECK("codecs: select Beethoven Bits", select_row("Beethoven Bits"));
    tap(HL_BTN_SELECT);
    CHECK("codecs: select the MP3 (Andante)", select_row("Andante"));
    size_t evm = hl_events_mark();
    uint64_t nz0 = hl_stats().audio_nonzero;
    tap(HL_BTN_SELECT);
    hl_run_ms(800);
    CHECK("codecs: MP3 opens the DAC at 44.1 kHz", ev_find(evm, HL_EV_AUDIO_INIT, 44100) >= 0);
    CHECK("codecs: MP3 decodes to real PCM", hl_stats().audio_nonzero - nz0 > 20000);
    CHECK("codecs: Now Playing names it", on_screen("Andante"));
    tap(HL_BTN_MENU);                         /* Now Playing -> tracklist */
    tap(HL_BTN_MENU);                         /* -> Albums                */
    CHECK("codecs: select Mono Mix", select_row("Mono Mix"));
    tap(HL_BTN_SELECT);
    CHECK("codecs: select the 22 kHz mono MP3", select_row("Mono Mp3"));
    evm = hl_events_mark();
    nz0 = hl_stats().audio_nonzero;
    tap(HL_BTN_SELECT);
    hl_run_ms(800);
    CHECK("codecs: 22 kHz mono re-opens the DAC at 22050 Hz",
          ev_find(evm, HL_EV_AUDIO_INIT, 22050) >= 0 && hl_stats().audio_rate == 22050);
    CHECK("codecs: mono MP3 decodes to real PCM", hl_stats().audio_nonzero - nz0 > 5000);
    /* (No underrun assertion here: these are 1-2 s tracks, and a queue that
     * runs out ends in a short read on the device too.) */
    tap(HL_BTN_MENU);
    tap(HL_BTN_MENU);

    CHECK("browse: select Long Player", select_row("Long Player"));
    tap(HL_BTN_SELECT);
    shot("detail");
    CHECK("browse: tracklist row 1", selected("Long One"));
    CHECK("browse: tracklist row 2", on_screen("Long Two"));
    CHECK("browse: track duration from the index", on_screen("1:30"));
    CHECK("browse: album summary", on_screen("2 tracks"));
    {
        /* Row 1 ("Long Two", unselected): its track number (regular 9) and
         * run time (bold 12) each centre their own figures on the row, as the
         * title's caps do (UI_ROW_BASE_FOR) — on the title's baseline they
         * hung 1.5 px and 1 px low. DET_LIST_Y0 = 108, ROW_H = 24 (main.c). */
        int ry = 108 + 24, row2 = 2 * ry + 24 - 1;
        hl_text_hit_t hn, hd;
        int fn = hl_find_text_in(hl_panel(), "2", ry, ry + 24, &hn);
        int fd = hl_find_text_in(hl_panel(), "1:30", ry, ry + 24, &hd);
        int cn = 2 * hn.y + hn.h - 1, cd = 2 * hd.y + hd.h - 1;
        CHECK("browse: track number centred on its row (within 1/2 px)",
              fn && cn - row2 <= 1 && row2 - cn <= 1);
        CHECK("browse: run time centred on its row (within 1/2 px)",
              fd && cd - row2 <= 1 && row2 - cd <= 1);
    }

    /* ---- 3. play ---------------------------------------------------------- */
    size_t ev0 = hl_events_mark();
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    shot("now_playing");
    s = hl_stats();
    CHECK("play: Now Playing screen", on_screen("Now Playing"));
    CHECK("play: Now Playing title", on_screen("Long One"));
    CHECK("play: Now Playing artist", on_screen("Test Tones"));
    CHECK("play: DAC opened at 44.1 kHz", ev_find(ev0, HL_EV_AUDIO_INIT, 44100) >= 0 &&
                                          s.audio_rate == 44100);
    CHECK("play: DAC running", s.audio_running && !s.audio_cold);
    uint64_t pulled0 = s.audio_frames_pulled;
    hl_run_ms(2000);
    s = hl_stats();
    CHECK("play: the source kept the DAC fed for 2 s (>= 1.9 s of frames)",
          s.audio_frames_pulled - pulled0 >= (uint64_t)(1.9 * 44100));
    CHECK("play: decoded PCM is not silence", s.audio_nonzero > 44100);
    CHECK("play: no underrun", s.audio_underruns == 0);
    uint16_t band[HL_W * BAND_H];
    hl_copy_rect(band, hl_panel(), 0, BAND_Y, HL_W, BAND_H);
    hl_run_ms(2000);
    CHECK("play: the transport band (clock + progress) moves while playing",
          !hl_rect_equal(band, hl_panel(), 0, BAND_Y, HL_W, BAND_H));

    /* ---- 4. bars ---------------------------------------------------------- */
    tap(HL_BTN_MENU);                         /* back to the tracklist */
    shot("detail_playing");
    CHECK("bars: back on the tracklist, still playing",
          on_screen("Long Two") && hl_stats().audio_running);
    size_t evb = hl_events_mark();
    int shapes = bar_shapes(16, 110);
    CHECK("bars: animate while playing (>= 4 shapes in 1.8 s)", shapes >= 4);
    CHECK("bars: animated by partial presents of their own box",
          ev_count(evb, HL_EV_PRESENT_RECT, BARS_RECT_ARG, 1) >= 8);

    tap(HL_BTN_RIGHT);                        /* RIGHT off a list: Now Playing */
    CHECK("bars: RIGHT from the tracklist opens Now Playing", on_screen("Now Playing"));
    size_t evp = hl_events_mark();
    tap(HL_BTN_PLAY);                         /* pause */
    s = hl_stats();
    CHECK("pause: PLAY on Now Playing stops the DAC",
          ev_find(evp, HL_EV_AUDIO_STOP, 0) >= 0 && !s.audio_running);
    uint64_t played_p = s.audio_frames_played;
    hl_copy_rect(band, hl_panel(), 0, BAND_Y, HL_W, BAND_H);
    hl_run_ms(3000);
    CHECK("pause: the transport band is frozen",
          hl_rect_equal(band, hl_panel(), 0, BAND_Y, HL_W, BAND_H));
    CHECK("pause: no frames clocked while paused",
          hl_stats().audio_frames_played == played_p);

    tap(HL_BTN_MENU);                         /* the tracklist, paused */
    shot("detail_paused");
    uint16_t frozen[BARS_W * BARS_H];
    hl_copy_rect(frozen, hl_panel(), BARS_X, BARS_Y, BARS_W, BARS_H);
    size_t evf = hl_events_mark();
    /* Sampled every 100 ms, not once at the end: the animation's period is
     * 760 ms, so a single later sample can land on the same shape by luck. */
    CHECK("bars: frozen while paused (v0.2.2): one shape across 3 s",
          bar_shapes(30, 100) == 1 &&
          hl_rect_equal(frozen, hl_panel(), BARS_X, BARS_Y, BARS_W, BARS_H));
    CHECK("bars: not re-pushed while paused",
          ev_count(evf, HL_EV_PRESENT_RECT, BARS_RECT_ARG, 1) == 0);
    /* A repaint of the row while paused must redraw the SAME frozen shape:
     * before bars_clock() it drew whatever phase the free-running timer was
     * at, so the glyph jumped on every repaint of a paused tracklist. */
    hl_scroll(1);
    hl_run_ms(1200);
    CHECK("bars: selection moved off the playing row", selected("Long Two"));
    hl_scroll(-1);
    hl_run_ms(1200);
    CHECK("bars: selection back on the playing row", selected("Long One"));
    CHECK("bars: a repaint while paused redraws the same frozen shape",
          hl_rect_equal(frozen, hl_panel(), BARS_X, BARS_Y, BARS_W, BARS_H));

    tap(HL_BTN_RIGHT);
    size_t evr = hl_events_mark();
    tap(HL_BTN_PLAY);                         /* resume */
    CHECK("resume: PLAY on Now Playing restarts the DAC",
          ev_find(evr, HL_EV_AUDIO_START, 0) >= 0 && hl_stats().audio_running);
    tap(HL_BTN_MENU);
    CHECK("bars: animate again after resume", bar_shapes(16, 110) >= 4);

    /* Pause for the rest of the walk (the idle-sleep rule needs a stopped
     * DAC). */
    tap(HL_BTN_RIGHT);
    tap(HL_BTN_PLAY);
    CHECK("pause: paused again for the idle phase", !hl_stats().audio_running);
    int paused_at = np_elapsed_s();
    CHECK("pause: Now Playing's clock is readable", paused_at > 0);
    if (g_report_fd >= 0) {
        (void)!write(g_report_fd, &paused_at, sizeof paused_at);
        close(g_report_fd);
    }

    /* ---- 5. settings ------------------------------------------------------ */
    tap(HL_BTN_MENU);                         /* Now Playing -> tracklist */
    tap(HL_BTN_MENU);                         /* -> Albums                */
    tap(HL_BTN_MENU);                         /* -> Music                 */
    tap(HL_BTN_MENU);                         /* -> main menu             */
    shot("main_with_track");
    CHECK("settings: back at the main menu", on_screen("Settings") && on_screen("Music"));
    CHECK("settings: select Settings", select_row("Settings"));
    tap(HL_BTN_SELECT);
    CHECK("settings: Settings list", hl_find_text_in(hl_panel(), "Settings", 0, 40, NULL));
    CHECK("settings: select Battery", select_row("Battery"));
    tap(HL_BTN_SELECT);
    shot("battery");
    CHECK("settings: Battery page renders", on_screen("On battery"));
    CHECK("settings: Battery page reads the PMU ADC", on_screen("3949 mV"));
    tap(HL_BTN_MENU);
    CHECK("settings: select About", select_row("About"));
    tap(HL_BTN_SELECT);
    shot("about");
    CHECK("settings: About header", hl_find_text_in(hl_panel(), "About", 0, 40, NULL));
    CHECK("settings: About names the device", on_screen("iPod 5.5G"));
    CHECK("settings: About counts songs", on_screen("SONGS") && on_screen("7"));
    CHECK("settings: About counts albums", on_screen("ALBUMS"));
    CHECK("settings: About shows the battery", on_screen("70%"));
    tap(HL_BTN_MENU);
    CHECK("settings: select Boot Details", select_row("Boot Details"));
    tap(HL_BTN_SELECT);
    shot("boot_details");
    CHECK("settings: Boot Details renders", on_screen("COLD BOOT"));
    tap(HL_BTN_MENU);
    CHECK("settings: back to About for the idle phase", select_row("About"));
    tap(HL_BTN_SELECT);
    CHECK("settings: on About", on_screen("iPod 5.5G"));
    CHECK("settings: firmware still running", hl_parked() == HL_RUNNING);

    /* ---- 6. idle sleep ---------------------------------------------------- */
    uint64_t t_input = hl_stats().now_us;     /* the last press ended here */
    size_t ev_idle = hl_events_mark();
    size_t log_idle = hl_log_len();
    log_wait_t w_sleep = { log_idle, "core: suspend: entering" };
    int slept = hl_run_until(pred_log, &w_sleep, 250, 140000);
    uint64_t t_sleep = hl_stats().now_us;
    shot("suspended");
    s = hl_stats();
    CHECK("idle: paused and untouched, the device suspends", slept);
    CHECK("idle: ...after the 120 s idle-sleep timeout, not before",
          t_sleep - t_input >= 119000000u && t_sleep - t_input <= 121500000u);
    CHECK("idle: said why", log_has_since(log_idle, "core: idle: nothing playing"));
    CHECK("idle: the codec was powered down across the pause",
          s.audio_cold && ev_find(0, HL_EV_AUDIO_SUSPEND, 0) >= 0);
    CHECK("idle: backlight dimmed then went out before the suspend",
          ev_find(ev_idle, HL_EV_BACKLIGHT, 0) >= 0 && s.backlight == 0);
    hl_run_ms(3000);
    s = hl_stats();
    CHECK("suspend: reached the low-power idle loop",
          log_has_since(log_idle, "core: suspend: idle loop"));
    CHECK("suspend: the panel holds a white frame", all_white());
    CHECK("suspend: the backlight is off", s.backlight == 0);
    CHECK("suspend: the drive is parked", s.ata_parked);
    CHECK("suspend: the 80 MHz boost is released", s.cpu_boost == 0);
    CHECK("suspend: nothing playing", !s.audio_running);
    CHECK("suspend: the final event-log block was forced out",
          log_has_since(log_idle, " final"));
    hl_stats_t s_in = s;
    size_t ev_in = hl_events_mark();
    hl_run_ms(60000);
    s = hl_stats();
    CHECK("suspend: a minute asleep presents nothing",
          s.presents_full == s_in.presents_full && s.presents_rect == s_in.presents_rect);
    CHECK("suspend: a minute asleep never spins the drive up",
          s.ata_spinups == s_in.ata_spinups && s.ata_parked);
    CHECK("suspend: still asleep (no spurious wake)",
          !log_has_since(log_idle, "core: suspend: wake") &&
          ev_find(ev_in, HL_EV_BACKLIGHT, 1) < 0);

    /* ---- 7. wake ---------------------------------------------------------- */
    size_t ev_w = hl_events_mark();
    size_t log_w = hl_log_len();
    hl_press(HL_BTN_MENU, 300);
    hl_run_ms(3000);
    shot("woke");
    s = hl_stats();
    CHECK("wake: a key wakes it", log_has_since(log_w, "core: suspend: wake"));
    long i_drive = ev_find(ev_w, HL_EV_ATA_WAKEUP, 0);
    long i_pres  = ev_find(ev_w, HL_EV_PRESENT_FULL, 0);
    long i_lit   = ev_find(ev_w, HL_EV_BACKLIGHT, 1);
    CHECK("wake: the drive is woken", i_drive >= 0);
    CHECK("wake: a full frame is presented", i_pres >= 0);
    CHECK("wake: the backlight comes back", i_lit >= 0 && s.backlight == 32);
    CHECK("wake: presented BEFORE the backlight is lit (no white flash)",
          i_pres >= 0 && i_lit >= 0 && i_pres < i_lit);
    CHECK("wake: the drive is up before the screen comes back",
          i_drive >= 0 && i_pres >= 0 && i_drive < i_pres);
    CHECK("wake: the panel is not left white", !all_white());
    CHECK("wake: the screen we left (About) is back, the wake press swallowed",
          on_screen("iPod 5.5G"));
    CHECK("wake: a track paused before the sleep stays paused", !s.audio_running);
    CHECK("wake: boosted again", s.cpu_boost >= 1);

    /* ---- 8. escalation to a PMU standby ----------------------------------- */
    hl_cfg.standby_accept = 1;
    size_t log_e = hl_log_len();
    log_wait_t w_sleep2 = { log_e, "core: suspend: entering" };
    CHECK("escalate: idles into a second suspend",
          hl_run_until(pred_log, &w_sleep2, 250, 125000));
    uint64_t t_s2 = hl_stats().now_us;
    size_t ev_e = hl_events_mark();
    for (int m = 0; m < 32 && hl_parked() == HL_RUNNING; m++) {
        hl_run_ms(60000);
    }
    long i_pmu = ev_find(ev_e, HL_EV_PMU_STANDBY, 0);
    size_t nev;
    const hl_ev_t *evs = hl_events(&nev);
    uint64_t asleep = i_pmu >= 0 ? evs[i_pmu].t_us - t_s2 : 0;
    CHECK("escalate: the PMU is told to power off", i_pmu >= 0);
    CHECK("escalate: ...after 30 minutes of suspend on battery",
          asleep >= 1800000000u && asleep <= 1802000000u);
    CHECK("escalate: the device is off", hl_parked() == HL_PARK_POWEROFF);
    CHECK("escalate: the standby path narrated it", log_has_since(log_e, "core: standby: entering"));

    /* ---- whole session ---------------------------------------------------- */
    s = hl_stats();
    CHECK("disk: the firmware wrote (config / evlog)", s.ata_writes > 0);
    CHECK("disk: every write inside a pre-allocated file, physically aligned",
          s.bad_writes == 0);
    CHECK("session: never panicked", hl_parked() != HL_PARK_PANIC);
    session_end("walk");
}

/*
 * Session 2: power back on from the disk session 1 left behind. Everything
 * here is state that only exists because session 1's writes reached the
 * platters at the right LBAs: the settings record's sequence, the event log's
 * FINAL flag, and the resume position. Then the resume-stall shape
 * (2026-09-13: a paused-at-boot track whose drive has parked must still start
 * when PLAY is pressed — the drive spins up under it).
 */
static _Noreturn void session_reboot(const char *disk, int paused_at_s)
{
    T.suite = "firmware-walk(2)";
    g_shot_n = 50;                          /* after session 1's shots */
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        exit(2);
    }
    shot("reboot");
    CHECK("reboot: running", hl_parked() == HL_RUNNING);
    CHECK("reboot: the settings record session 1 wrote is the one loaded",
          log_has("core: cfg load 00000001 writable 00000001 seq ") &&
          !log_has("core: cfg load 00000001 writable 00000001 seq 00000001 "));
    CHECK("reboot: the event log knows the last session ended in a FINAL flush",
          log_has(" prev final"));
    CHECK("reboot: the resumed track is loaded (status strip)",
          hl_find_text_in(hl_panel(), "Long One", 0, 16, NULL));
    CHECK("reboot: ...and offered on the main menu", on_screen("Now Playing"));
    CHECK("reboot: it comes up paused", !hl_stats().audio_running);

    /* Leave it until the drive parks, so PLAY has to spin it up. */
    size_t ev_park = hl_events_mark();
    for (int i = 0; i < 60 && !hl_stats().ata_parked; i++) hl_run_ms(1000);
    CHECK("reboot: the drive parks while nothing plays", hl_stats().ata_parked &&
          ev_find(ev_park, HL_EV_ATA_STANDBY, 0) >= 0);

    CHECK("reboot: select Now Playing", select_row("Now Playing"));
    tap(HL_BTN_SELECT);
    shot("reboot_now_playing");
    CHECK("reboot: Now Playing shows the resumed track", on_screen("Long One"));
    int at = np_elapsed_s();
    CHECK("reboot: the position is where session 1 paused (+-1 s)",
          paused_at_s > 0 && at >= paused_at_s - 1 && at <= paused_at_s + 1);

    size_t ev_play = hl_events_mark();
    uint64_t pulled0 = hl_stats().audio_frames_pulled;
    tap(HL_BTN_PLAY);
    hl_run_ms(2500);
    hl_stats_t s = hl_stats();
    /* Opening Now Playing may already have spun it up (the cover read);
     * either way the parked drive had to come back under the paused track. */
    CHECK("reboot: the parked drive is spun back up for the track",
          ev_find(ev_park, HL_EV_ATA_SPINUP, 0) >= 0);
    CHECK("reboot: the DAC is started by PLAY", ev_find(ev_play, HL_EV_AUDIO_START, 0) >= 0);
    CHECK("reboot: ...and the track plays (no resume stall)",
          s.audio_running && s.audio_frames_pulled - pulled0 >= (uint64_t)(1.5 * 44100));
    CHECK("reboot: no underrun on the cold start", s.audio_underruns == 0);
    int at2 = np_elapsed_s();
    CHECK("reboot: it plays on FROM the resumed position", at2 >= paused_at_s + 1);

    /* Past what the paused pump stocked: "Long One" is ~16 MB of VERBATIM
     * FLAC, twice the anti-skip buffer, so 40 s more needs refill bursts and
     * the drive spinning up under a playing track. */
    uint64_t pulled1 = hl_stats().audio_frames_pulled;
    size_t ev_run = hl_events_mark();
    hl_run_ms(40000);
    s = hl_stats();
    CHECK("reboot: 40 s more of the track reach the DAC",
          s.audio_frames_pulled - pulled1 >= (uint64_t)(39.5 * 44100));
    printf("reboot: in 40 s of play: %d spin-ups, %d parks, underruns %u\n",
           ev_count(ev_run, HL_EV_ATA_SPINUP, 0, 0),
           ev_count(ev_run, HL_EV_ATA_STANDBY, 0, 0), s.audio_underruns);
    CHECK("reboot: ...across refill bursts that park and spin the drive",
          ev_find(ev_run, HL_EV_ATA_STANDBY, 0) >= 0 ||
          ev_find(ev_run, HL_EV_ATA_SPINUP, 0) >= 0);
    CHECK("reboot: ...without one underrun", s.audio_underruns == 0);
    CHECK("reboot: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("reboot: never panicked", hl_parked() != HL_PARK_PANIC);
    session_end("reboot");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges [SHOT_DIR]\n", argv[0]);
        return 2;
    }
    g_shots = argc > 3 ? argv[3] : NULL;

    char disk[] = "/tmp/firmware_walk_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);

    /* Each session is its own process: the firmware's statics start from
     * .bss again, as they do on a real power-on, and session 2 boots the
     * disk session 1 wrote. Session 1 reports where it paused down a pipe. */
    int pfd[2];
    if (pipe(pfd) != 0) return 2;
    fflush(NULL);
    pid_t p1 = fork();
    if (p1 == 0) {
        close(pfd[0]);
        g_report_fd = pfd[1];
        session_walk(disk, argv[2]);
    }
    close(pfd[1]);
    int paused_at = -1;
    if (read(pfd[0], &paused_at, sizeof paused_at) != (ssize_t)sizeof paused_at) {
        paused_at = -1;
    }
    close(pfd[0]);
    int st1 = 0, st2 = 0;
    waitpid(p1, &st1, 0);

    fflush(NULL);
    pid_t p2 = fork();
    if (p2 == 0) {
        session_reboot(disk, paused_at);
    }
    waitpid(p2, &st2, 0);
    unlink(disk);

    int ok1 = WIFEXITED(st1) && WEXITSTATUS(st1) == 0;
    int ok2 = WIFEXITED(st2) && WEXITSTATUS(st2) == 0;
    if (!ok1) fprintf(stderr, "firmware-walk: session 1 (walk) FAILED\n");
    if (!ok2) fprintf(stderr, "firmware-walk: session 2 (reboot) FAILED\n");
    return ok1 && ok2 ? 0 : 1;
}
