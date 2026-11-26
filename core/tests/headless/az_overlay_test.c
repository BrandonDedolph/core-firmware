/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/az_overlay_test.c — the A-Z letter plate, raised by a fast
 * spin on the real firmware's long lists.
 *
 * ui/wheel.c (the letter-mode latch) and ui/letterindex.c (the run index)
 * have unit tests. What they never run is kernel/main.c's half: the per-screen
 * initial (screen_initial_raw: a title on Songs, the name past a leading
 * "The " on Artists), the index kept up to date with whatever list is up
 * (letters_ensure, keyed on screen, depth, row count AND g_list_epoch), the
 * letter the plate shows (list_sel_initial), the letter step the wheel is
 * handed (list_letter_step_idx), the plate itself (az_overlay_render) and the
 * clock the hold window runs on (wheel_clock). The stock headless disk has
 * seven songs, under LETTERIDX_MIN_ROWS (48), so nothing else in the suite
 * ever shows the plate. This boots the --library az disk
 * (tests/scripts/make_headless_disk.py az_library):
 *
 *   Songs    146 rows: five '#' ("(Stew Intro)" first), A at "Aa Soup 01",
 *            ..., H only "Harbor Song", nothing under I, J at "Jade Song"
 *   Artists  48 rows (exactly the threshold): '#' "4 Seasons Trio", A at
 *            "Alphabet Soup", ..., H "Harbor Lights", J "Jade River", K at
 *            "The Kestrels" (filed past its "The ")
 *   Alphabet Soup / Alphabet Stew, All Songs: 48 rows each, the same screen
 *            and depth, different runs (Soup A,B,C,D at 0,3,6,8; Stew
 *            #,B,C,D at 0,3,6,9)
 *   Genres   47 rows (one under the threshold)
 *
 * The plate is read off the panel: a 66x66 plate in the panel's middle, its
 * own colour (PAL_PLATE) solid down its left edge, and the letter on it found
 * with the firmware's own glyphs (hl_screen.h) in a copy of just that square.
 *
 *   songs    a slow detent moves a row and raises nothing; a fast spin at the
 *            top raises '#' (digits and punctuation) on "(Stew Intro)"; each
 *            detent then steps ONE letter, A..H, then J (I is empty), the
 *            cursor on each letter's first row; the plate outlives a 300 ms
 *            pause between detents and 800 ms after the last, is gone 1.2 s
 *            after it, and leaves the cursor on "Jade Song"; the next slow
 *            detent is one row again
 *   artists  the same on 48 rows, through H, J and to K: "The Kestrels"
 *   epoch    Soup's All Songs letter-stepped, then Stew's — same screen,
 *            depth and row count, so only g_list_epoch tells the index the
 *            CONTENTS changed: '#' on "(Stew Intro)", then B, C and D landing
 *            on Stew's heads (a stale Soup index shows 'A' and lands D on
 *            "Cc Stew 47")
 *   short    Genres (47 rows) spun as fast: no plate
 *
 * Every case is its own power-on on a fresh copy of the disk.
 *
 * Usage: az_overlay_test DISK_AZ.img DISK_AZ.img.ranges [SHOT_DIR]
 */

#include "headless.h"
#include "hl_screen.h"
#include "ui/palette.h"            /* g_pal[PAL_PLATE]: the plate's colour */
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "az-overlay", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

static const char *g_shots;
static const char *g_case = "";
static int g_shot_n;

static void shot(const char *name)
{
    if (!g_shots) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/az_overlay_%s_%02d_%s.bmp",
             g_shots, g_case, g_shot_n++, name);
    (void)hl_write_bmp(path);
}

/* ---- the panel (as search_walk_test.c's) --------------------------------- */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int header(const char *s) { return hl_find_text_in(hl_panel(), s, 0, 40, NULL); }
/* `s` is the selected row's title: found inside the selection bar (the dark
 * band at the list's left edge), starting the row, light on dark. Searching
 * the bar alone is what keeps "Songs" from matching the plain row "Shuffle
 * Songs", and the left edge what keeps it from matching a selected one. */
static int selected(const char *s)
{
    const uint16_t *p = hl_panel();
    int y0 = -1, y1 = -1;
    for (int y = 40; y < HL_H; y++) {
        int dark = lum(p[y * HL_W + 10]) < 60;
        if (dark && y0 < 0) y0 = y;
        if (!dark && y0 >= 0) { y1 = y; break; }
    }
    if (y0 < 0) return 0;
    if (y1 < 0) y1 = HL_H;
    hl_text_hit_t h;
    return hl_find_text_in(p, s, y0, y1, &h) && h.x <= 20 &&
           lum(h.ink) > lum(h.bg);
}
static void tap(unsigned btn) { hl_tap(btn, 500); }
static int pred_header(void *a) { return header((const char *)a); }

/* Wheel (slowly) to the top of a short list, then down a row at a time. */
static int select_row(const char *s)
{
    if (selected(s)) return 1;
    hl_scroll(-12);
    hl_run_ms(12 * 150 + 400);
    for (int i = 0; i < 12; i++) {
        if (selected(s)) return 1;
        hl_scroll(1);
        hl_run_ms(450);
    }
    return selected(s);
}

/* ---- the plate ------------------------------------------------------------ */

/* az_overlay_render's geometry: 66x66, centred. */
#define PLATE_W 66
#define PLATE_H 66
#define PLATE_X ((HL_W - PLATE_W) / 2)
#define PLATE_Y ((HL_H - PLATE_H) / 2)

/* Up: the plate's colour, solid, in a strip down its left side (clear of the
 * rounded corners and of the letter). A list row there is text on linen or
 * on the selection bar, never a solid 8x40 of PAL_PLATE. */
static int plate_up(void)
{
    int n = hl_count_color(hl_panel(), g_pal[PAL_PLATE],
                           PLATE_X + 4, PLATE_Y + 13, 8, 40);
    return n == 8 * 40;
}

/* The letter on the plate: the one glyph of '#', 'A'..'Z' found in a frame
 * that is the plate and nothing else; 0 with no plate, '?' when no glyph or
 * more than one matches. */
static char plate_letter(void)
{
    if (!plate_up()) return 0;
    static uint16_t f[HL_W * HL_H];
    for (int i = 0; i < HL_W * HL_H; i++) f[i] = g_pal[PAL_PLATE];
    const uint16_t *p = hl_panel();
    for (int y = PLATE_Y; y < PLATE_Y + PLATE_H; y++) {
        memcpy(&f[y * HL_W + PLATE_X], &p[y * HL_W + PLATE_X],
               PLATE_W * sizeof f[0]);
    }
    static const char cand[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char got = 0;
    for (const char *c = cand; *c; c++) {
        char s[2] = { *c, 0 };
        if (hl_find_text(f, s, NULL)) {
            if (got) return '?';
            got = *c;
        }
    }
    return got ? got : '?';
}

static void expect_plate(const char *what, char want)
{
    char got = plate_letter();
    char l[160];
    snprintf(l, sizeof l, "%s: the plate is up showing '%c'", what, want);
    CHECK(l, got == want);
    if (got != want) {
        fprintf(stderr, "  (%s: plate %s '%c')\n", what,
                got ? "shows" : "is down,", got ? got : '-');
    }
}

static void expect_row(const char *what, const char *row)
{
    char l[160];
    snprintf(l, sizeof l, "%s: the cursor is on \"%s\"", what, row);
    CHECK(l, selected(row));
}

/* A fast spin: `ticks` raw ticks at 200 ticks/s (well past ui/wheel.c's
 * letter threshold), then 50 ms more. */
static void spin(int ticks)
{
    hl_spin(ticks, 5000);
    hl_run_ms((uint32_t)(ticks < 0 ? -ticks : ticks) * 5u + 50u);
}

/* One slow detent (4 ticks at once) and 300 ms: past WHEEL_IDLE_US, so the
 * SPEED has reset, but inside WHEEL_AZ_HOLD_LETTER, so letter mode holds. */
static void detent(int dir)
{
    hl_scroll(dir);
    hl_run_ms(300);
}

/* Spin at the top of the list (the cursor cannot move: only the plate can
 * change), then detent forward through `letters`, each landing on `rows[i]`. */
static void walk_letters(const char *what, char top, const char *top_row,
                         const char *letters, const char *const *rows)
{
    char l[96];
    spin(-16);
    shot("spun");
    snprintf(l, sizeof l, "%s spin at the top", what);
    expect_plate(l, top);
    expect_row(l, top_row);
    for (int i = 0; letters[i]; i++) {
        detent(+1);
        snprintf(l, sizeof l, "%s detent %d", what, i + 1);
        expect_plate(l, letters[i]);
        expect_row(l, rows[i]);
    }
    shot("stepped");
}

/* After the last detent: up at 300 + 800 ms, down by 300 + 1700 ms. */
static void hold_then_clear(const char *what, const char *row)
{
    char l[128];
    hl_run_ms(800);
    snprintf(l, sizeof l, "%s: still up 1.1 s after the last detent", what);
    CHECK(l, plate_up());
    hl_run_ms(900);
    shot("cleared");
    snprintf(l, sizeof l, "%s: gone 2 s after the last detent", what);
    CHECK(l, !plate_up());
    snprintf(l, sizeof l, "%s: cleared", what);
    expect_row(l, row);
}

/* ---- boot and disk -------------------------------------------------------- */

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

static _Noreturn void session_end(void)
{
    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: fake time %.1f s\n", g_case, (double)s.now_us / 1e6);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);                     /* the firmware thread is frozen mid-wait */
}

/* Boot, main menu -> Music -> `item`, whose screen is headed `hdr`. */
static void boot_to(const char *disk, const char *ranges, const char *item,
                    const char *hdr)
{
    CHECK("image: the pre-allocated files' extents are known", load_ranges(ranges) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        _exit(2);
    }
    tap(HL_BTN_SELECT);                               /* main menu -> Music */
    char l[96];
    snprintf(l, sizeof l, "nav: Music > %s", item);
    CHECK(l, select_row(item));
    tap(HL_BTN_SELECT);
    snprintf(l, sizeof l, "nav: the %s screen", hdr);
    CHECK(l, hl_run_until(pred_header, (void *)hdr, 100, 3000));
    hl_run_ms(1500);                                  /* let any wheel state age out */
    shot("list");
}

/* ---- the cases ------------------------------------------------------------ */

static void songs_case(void)
{
    CHECK("songs: on the first row", selected("(Stew Intro)"));
    detent(+1);
    CHECK("songs: a slow detent moves one row", selected("07 Stew"));
    CHECK("songs: ...and raises no plate", !plate_up());
    detent(-1);
    CHECK("songs: back on the first row", selected("(Stew Intro)"));
    hl_run_ms(1500);

    static const char *const rows[] = {
        "Aa Soup 01", "Bb Soup 02", "Cc Soup 03", "Dd Soup 04", "Echo Song",
        "Fern Song", "Gg Soup 07", "Harbor Song", "Jade Song",
    };
    walk_letters("songs", '#', "(Stew Intro)", "ABCDEFGHJ", rows);
    hold_then_clear("songs", "Jade Song");
    detent(+1);
    CHECK("songs: after the plate, a slow detent is one row again",
          selected("Jj Soup 08"));
    CHECK("songs: ...with no plate", !plate_up());
}

static void artists_case(void)
{
    CHECK("artists: on the first row", selected("4 Seasons Trio"));
    static const char *const rows[] = {
        "Alphabet Soup", "Bell Tower", "Cinder", "Delta Nine", "Echo Park",
        "Fern Gully", "Gold Coast", "Harbor Lights", "Jade River",
        "The Kestrels",
    };
    walk_letters("artists", '#', "4 Seasons Trio", "ABCDEFGHJK", rows);
    hold_then_clear("artists", "The Kestrels");
}

/* Artists > `artist` > All Songs, from the Artists screen with the cursor on
 * the row above it (or on it). */
static void open_all_songs(const char *artist)
{
    char l[96];
    snprintf(l, sizeof l, "nav: Artists > %s", artist);
    if (!selected(artist)) detent(+1);
    CHECK(l, selected(artist));
    tap(HL_BTN_SELECT);
    snprintf(l, sizeof l, "nav: %s's albums, All Songs selected", artist);
    CHECK(l, header(artist) && selected("All Songs"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    snprintf(l, sizeof l, "nav: %s's All Songs", artist);
    CHECK(l, header(artist) && !selected("All Songs"));
    hl_run_ms(1500);
}

static void epoch_case(void)
{
    /* Soup: builds the index for (Songs, depth 0, 48 rows). */
    detent(+1);
    detent(+1);
    hl_run_ms(1500);
    open_all_songs("Alphabet Soup");
    static const char *const soup[] = { "Bb Soup 02", "Cc Soup 03", "Dd Soup 04" };
    walk_letters("soup", 'A', "Aa Soup 01", "BCD", soup);
    hold_then_clear("soup", "Dd Soup 04");

    /* Back to Artists with nothing but MENU and one slow detent: none of it
     * asks for a letter, so the index is still Soup's when Stew's list is up
     * — same screen, same depth, same 48 rows. Only the epoch differs. */
    tap(HL_BTN_MENU);
    tap(HL_BTN_MENU);
    CHECK("nav: back on Artists", header("Artists"));
    hl_run_ms(1500);
    open_all_songs("Alphabet Stew");
    static const char *const stew[] = { "Bb Stew 04", "Cc Stew 05", "Dd Stew 06" };
    walk_letters("stew (the index rebuilt)", '#', "(Stew Intro)", "BCD", stew);
    hold_then_clear("stew", "Dd Stew 06");
}

static void short_case(void)
{
    CHECK("short: Genres, on the first row", selected("4"));
    spin(-16);
    CHECK("short: 47 rows, spun at the top: no plate", !plate_up());
    hl_run_ms(1500);
    spin(32);
    shot("spun");
    CHECK("short: spun forward: no plate", !plate_up());
    CHECK("short: ...and the cursor moved by rows", !selected("4"));
}

typedef struct {
    const char *name, *item, *hdr;
    void (*run)(void);
} case_t;

static const case_t CASES[] = {
    { "songs",   "Songs",   "Songs",   songs_case },
    { "artists", "Artists", "Artists", artists_case },
    { "epoch",   "Artists", "Artists", epoch_case },
    { "short",   "Genres",  "Genres",  short_case },
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK_AZ.img DISK_AZ.img.ranges [SHOT_DIR]\n",
                argv[0]);
        return 2;
    }
    g_shots = argc > 3 ? argv[3] : NULL;
    int ok = 1;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char disk[] = "/tmp/az_overlay_XXXXXX";
        int fd = mkstemp(disk);
        if (fd < 0 || copy_disk(argv[1], disk) != 0) {
            fprintf(stderr, "cannot copy %s\n", argv[1]);
            return 2;
        }
        close(fd);
        fflush(NULL);
        pid_t p = fork();
        if (p == 0) {
            g_case = CASES[i].name;
            T.suite = CASES[i].name;
            boot_to(disk, argv[2], CASES[i].item, CASES[i].hdr);
            CASES[i].run();
            session_end();
        }
        int st = 0;
        waitpid(p, &st, 0);
        unlink(disk);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fprintf(stderr, "az-overlay: %s FAILED\n", CASES[i].name);
            ok = 0;
        }
    }
    return ok ? 0 : 1;
}
