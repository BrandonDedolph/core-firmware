/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/list_input_test.c — the plain list screens' wheel, SELECT
 * and MENU, on rows other than the first.
 *
 * firmware-walk and the other headless tests reach every list screen, but
 * almost always to SELECT its first row or a row whose position does not
 * matter: nothing selects a queue row, and nothing opens a genre other than
 * the first (the stock disk has one). So a SELECT that read the wrong cursor,
 * a MENU that left the screen beneath it with a reset cursor, a wheel that
 * clamped one row short, or the Queue's SELECT that forgot to return to Now
 * Playing would all pass. This drives the real run_ui input switch on the
 * --library az disk (tests/scripts/make_headless_disk.py az_library:
 * Artists 48 rows, Songs 146, Genres 47) and checks, screen by screen, the
 * firmware's own cursor and stack (read while it is frozen) and the row the
 * panel shows highlighted:
 *
 *   main menu  the wheel clamps at Music and at the last visible row
 *              (Settings while idle, Now Playing while playing); SELECT on
 *              Playlists / Settings / Now Playing opens that screen and MENU
 *              comes back with the cursor on it; MENU on the root stays put
 *   Music      clamps at Playlists and at Audiobooks; MENU from Artists,
 *              Songs and Genres lands on that row; MENU back to the main
 *              menu leaves it on Music
 *   Artists    clamps at row 0 and 47; SELECT on "Test Tones" opens the album
 *              list filtered to that artist (one album, Long Player); MENU
 *              comes back with the cursor still on it
 *   Songs      clamps at row 0 and 145; SELECT on "Long Two" plays that song
 *              of the 146-song queue and opens Now Playing; MENU back lands
 *              on Long Two
 *   Genres     clamps at row 0 and 46; SELECT on "Long" lists exactly that
 *              genre's two songs; MENU comes back with the cursor on it
 *   Queue      (held SELECT on Now Playing, Long One of the Long genre
 *              playing) opens on the playing row and clamps at both ends;
 *              SELECT on Long Two jumps playback to it and returns to Now
 *              Playing (above the Songs list, not a second Queue); MENU
 *              returns without jumping
 *
 * Usage: list_input_test DISK_AZ.img DISK_AZ.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "player/player.h"          /* read-only, while the firmware is frozen */
#include "app/nav.h"                /* scr_cur, g_scr, g_scr_n               */
#include "app/lists.h"              /* the cursors, MM_*, MU_*               */
#include "app/library.h"            /* g_artists, g_genres, g_songview       */
#include "app/albums.h"             /* g_artist_filter                       */
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "list-input", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* ---- helpers --------------------------------------------------------------- */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
/* `s` is drawn as the highlighted row (light ink on dark) below the header.
 * The panel is searched a row-sized band at a time, so a plain match on
 * another row cannot hide the highlighted one: on the Music menu "Songs" is
 * also the tail of "Shuffle Songs", the row below it. */
static int selected(const char *s)
{
    hl_text_hit_t h;
    for (int y0 = 40; y0 < HL_H; y0 += 6) {
        int y1 = y0 + 30 < HL_H ? y0 + 30 : HL_H;
        if (hl_find_text_in(hl_panel(), s, y0, y1, &h) && lum(h.ink) > lum(h.bg)) {
            return 1;
        }
    }
    return 0;
}
/* The Music menu's Songs row, and not Shuffle Songs. */
static int songs_row_selected(void)
{
    return selected("Songs") && !selected("Shuffle Songs");
}
/* `rows` detents, at rest (one row per 150 ms), then let the screen settle. */
static void wheel(int rows)
{
    hl_scroll(rows);
    hl_run_ms((uint32_t)(rows < 0 ? -rows : rows) * 150u + 400u);
}
static void tap(unsigned btn, uint32_t settle) { hl_tap(btn, settle); }

/* The stack is exactly `n` deep with `top` on it and `below` beneath. */
static int stack_is(int n, screen_t below, screen_t top)
{
    return g_scr_n == n && scr_cur() == top && g_scr[n - 2] == below;
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

static int find_artist(const char *name)
{
    for (int i = 0; i < g_artists_n; i++) {
        if (strcmp(g_artists[i].name, name) == 0) return i;
    }
    return -1;
}
static int find_genre(const char *name)
{
    for (int i = 0; i < g_genres_n; i++) {
        if (strcmp(g_genres[i], name) == 0) return i;
    }
    return -1;
}
static int find_song(const char *title)
{
    for (int i = 0; i < g_songview_n; i++) {
        if (strcmp(g_songs[g_songview[i]].title, title) == 0) return i;
    }
    return -1;
}
/* The playing queue entry is song `title` (its file stem, as queued). */
static int playing_song(const char *title)
{
    int cur = player_queue_current();
    for (int i = 0; i < g_songs_n; i++) {
        if (strcmp(g_songs[i].title, title) == 0) {
            return strcmp(player_queue_name(cur), g_songs[i].file) == 0;
        }
    }
    return 0;
}

/* ---- the screens ----------------------------------------------------------- */

static void main_menu_idle(void)
{
    CHECK("main: boots to the main menu, cursor on Music",
          g_scr_n == 1 && scr_cur() == SCR_MENU && g_main_sel == MM_MUSIC &&
          selected("Music"));
    wheel(-3);
    CHECK("main: the wheel clamps at the top (Music)",
          g_main_sel == MM_MUSIC && selected("Music"));
    wheel(8);
    CHECK("main: ...and at the last row shown idle (Settings; no Now Playing)",
          g_main_sel == MM_SETTINGS && selected("Settings"));
    wheel(-3);
    CHECK("main: three rows up is Playlists",
          g_main_sel == MM_PLAYLISTS && selected("Playlists"));

    tap(HL_BTN_SELECT, 800);
    CHECK("main: SELECT on Playlists opens Playlists",
          stack_is(2, SCR_MENU, SCR_PLAYLISTS));
    tap(HL_BTN_MENU, 500);
    CHECK("main: MENU comes back to the main menu, cursor on Playlists",
          g_scr_n == 1 && scr_cur() == SCR_MENU && g_main_sel == MM_PLAYLISTS &&
          selected("Playlists"));

    wheel(3);
    tap(HL_BTN_SELECT, 500);
    CHECK("main: SELECT on Settings opens Settings",
          stack_is(2, SCR_MENU, SCR_SETTINGS));
    tap(HL_BTN_MENU, 500);
    CHECK("main: MENU comes back, cursor on Settings",
          g_scr_n == 1 && scr_cur() == SCR_MENU && g_main_sel == MM_SETTINGS &&
          selected("Settings"));
    tap(HL_BTN_MENU, 500);
    CHECK("main: MENU on the root stays there, cursor unmoved",
          g_scr_n == 1 && scr_cur() == SCR_MENU && g_main_sel == MM_SETTINGS);

    wheel(-4);
    CHECK("main: back up to Music", g_main_sel == MM_MUSIC);
    tap(HL_BTN_SELECT, 500);
}

static void music_menu(void)
{
    CHECK("music: SELECT on Music opens the Music menu on Albums",
          stack_is(2, SCR_MENU, SCR_MUSIC) && g_music_sel == MU_ALBUMS &&
          selected("Albums"));
    wheel(-5);
    CHECK("music: the wheel clamps at the top (Playlists)",
          g_music_sel == MU_PLAYLISTS && selected("Playlists"));
    wheel(12);
    CHECK("music: ...and at the bottom (Audiobooks)",
          g_music_sel == MU_COUNT - 1 && selected("Audiobooks"));
    wheel(MU_ARTISTS - (MU_COUNT - 1));
    CHECK("music: up to Artists", g_music_sel == MU_ARTISTS && selected("Artists"));
}

static void artists(void)
{
    tap(HL_BTN_SELECT, 800);
    CHECK("artists: SELECT on Artists opens Artists at the top",
          stack_is(3, SCR_MUSIC, SCR_ARTISTS) && g_artist_sel == 0);
    CHECK("artists: 48 artists on the az disk", g_artists_n == 48);
    wheel(-3);
    CHECK("artists: the wheel clamps at row 0", g_artist_sel == 0);
    wheel(g_artists_n + 6);
    CHECK("artists: ...and at the last row", g_artist_sel == g_artists_n - 1);

    int k = find_artist("Test Tones");
    CHECK("artists: Test Tones is listed, not first", k > 0);
    wheel(k - (g_artists_n - 1));
    CHECK("artists: the cursor on Test Tones",
          g_artist_sel == k && selected("Test Tones"));
    tap(HL_BTN_SELECT, 800);
    CHECK("artists: SELECT opens the album list filtered to Test Tones",
          stack_is(4, SCR_ARTISTS, SCR_BROWSER) &&
          strcmp(g_artist_filter, "Test Tones") == 0);
    /* Test Tones has one of the library's 48 albums: the list is exactly that
     * one, not all of them (an unfiltered albumview_build). */
    CHECK("artists: ...which lists exactly its one album, Long Player",
          g_albums_n > 1 && g_albumview_n == 1 &&
          strstr(g_albums[g_albumview[0]].folder, "Long Player") != NULL &&
          hl_find_text_in(hl_panel(), "Long Player", 40, HL_H, NULL));
    tap(HL_BTN_MENU, 500);
    CHECK("artists: MENU comes back to Artists, cursor still on Test Tones",
          stack_is(3, SCR_MUSIC, SCR_ARTISTS) && g_artist_sel == k &&
          selected("Test Tones"));
    tap(HL_BTN_MENU, 500);
    CHECK("artists: MENU again: the Music menu, cursor on Artists",
          stack_is(2, SCR_MENU, SCR_MUSIC) && g_music_sel == MU_ARTISTS &&
          selected("Artists"));
}

static void songs(void)
{
    wheel(MU_SONGS - MU_ARTISTS);
    CHECK("songs: the Music cursor on Songs", g_music_sel == MU_SONGS && songs_row_selected());
    tap(HL_BTN_SELECT, 800);
    CHECK("songs: SELECT opens all 146 songs at the top",
          stack_is(3, SCR_MUSIC, SCR_SONGS) && g_songview_n == 146 && g_song_sel == 0);
    wheel(-3);
    CHECK("songs: the wheel clamps at row 0", g_song_sel == 0);
    wheel(g_songview_n + 4);
    CHECK("songs: ...and at the last row", g_song_sel == g_songview_n - 1);

    int k = find_song("Long Two");
    CHECK("songs: Long Two is listed, not first", k > 0);
    wheel(k - (g_songview_n - 1));
    CHECK("songs: the cursor on Long Two", g_song_sel == k && selected("Long Two"));
    tap(HL_BTN_SELECT, 1500);
    CHECK("songs: SELECT (a tap) opens Now Playing",
          stack_is(4, SCR_SONGS, SCR_NOWPLAYING));
    CHECK("songs: ...playing Long Two out of the whole list",
          player_active() && player_queue_len() == 146 && playing_song("Long Two"));
    tap(HL_BTN_MENU, 500);
    CHECK("songs: MENU comes back to Songs, cursor still on Long Two",
          stack_is(3, SCR_MUSIC, SCR_SONGS) && g_song_sel == k && selected("Long Two"));
    tap(HL_BTN_MENU, 500);
    CHECK("songs: MENU again: the Music menu, cursor on Songs",
          stack_is(2, SCR_MENU, SCR_MUSIC) && g_music_sel == MU_SONGS &&
          songs_row_selected());
}

static int g_long;                  /* the "Long" genre's row */

static void genres(void)
{
    wheel(MU_GENRES - MU_SONGS);
    CHECK("genres: the Music cursor on Genres", g_music_sel == MU_GENRES && selected("Genres"));
    tap(HL_BTN_SELECT, 800);
    CHECK("genres: SELECT opens Genres at the top",
          stack_is(3, SCR_MUSIC, SCR_GENRES) && g_genre_sel == 0);
    CHECK("genres: 47 genres on the az disk", g_genres_n == 47);
    wheel(-3);
    CHECK("genres: the wheel clamps at row 0", g_genre_sel == 0);
    wheel(g_genres_n + 6);
    CHECK("genres: ...and at the last row", g_genre_sel == g_genres_n - 1);

    int k = g_long = find_genre("Long");
    CHECK("genres: Long is listed, not first", k > 0);
    wheel(k - (g_genres_n - 1));
    CHECK("genres: the cursor on Long", g_genre_sel == k && selected("Long"));
    tap(HL_BTN_SELECT, 800);
    CHECK("genres: SELECT opens that genre's songs",
          stack_is(4, SCR_GENRES, SCR_SONGS) && g_song_sel == 0);
    int only_long = g_songview_n == 2;
    for (int i = 0; i < g_songview_n; i++) {
        only_long = only_long && g_songs[g_songview[i]].genre == k;
    }
    CHECK("genres: ...exactly Long's two songs, Long One first",
          only_long && find_song("Long One") == 0 && find_song("Long Two") == 1 &&
          selected("Long One"));
    tap(HL_BTN_MENU, 500);
    CHECK("genres: MENU comes back to Genres, cursor still on Long",
          stack_is(3, SCR_MUSIC, SCR_GENRES) && g_genre_sel == k && selected("Long"));
    tap(HL_BTN_SELECT, 800);
    CHECK("genres: SELECT on Long again", stack_is(4, SCR_GENRES, SCR_SONGS));
}

static void queue(void)
{
    /* On Long's songs, Long One selected: play it, a two-song queue. */
    tap(HL_BTN_SELECT, 1500);
    CHECK("queue: Long One plays, Long's two songs queued",
          stack_is(5, SCR_SONGS, SCR_NOWPLAYING) && player_queue_len() == 2 &&
          player_queue_current() == 0 && playing_song("Long One"));

    hl_press(HL_BTN_SELECT, 700);               /* past SEL_HOLD_US */
    hl_run_ms(800);
    CHECK("queue: a held SELECT opens the Queue on the playing row",
          stack_is(6, SCR_NOWPLAYING, SCR_QUEUE) && g_queue_sel == 0);
    wheel(-3);
    CHECK("queue: the wheel clamps at row 0", g_queue_sel == 0);
    wheel(4);
    CHECK("queue: ...and at the last row", g_queue_sel == 1);

    tap(HL_BTN_SELECT, 1000);
    CHECK("queue: SELECT on row 1 returns to Now Playing (not the Queue)",
          stack_is(5, SCR_SONGS, SCR_NOWPLAYING));
    CHECK("queue: ...playing Long Two, the row selected",
          player_queue_current() == 1 && playing_song("Long Two") &&
          hl_stats().audio_running);

    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(800);
    CHECK("queue: held again, the Queue opens on Long Two",
          stack_is(6, SCR_NOWPLAYING, SCR_QUEUE) && g_queue_sel == 1);
    wheel(-1);
    CHECK("queue: cursor up to Long One", g_queue_sel == 0);
    tap(HL_BTN_MENU, 500);
    CHECK("queue: MENU returns to Now Playing without jumping",
          stack_is(5, SCR_SONGS, SCR_NOWPLAYING) && player_queue_current() == 1 &&
          playing_song("Long Two"));

    /* Back out the way we came, each cursor where it was left. */
    tap(HL_BTN_MENU, 500);
    CHECK("queue: MENU: Long's songs, cursor on Long One",
          stack_is(4, SCR_GENRES, SCR_SONGS) && g_song_sel == 0 && selected("Long One"));
    tap(HL_BTN_MENU, 500);
    CHECK("queue: MENU: Genres, cursor on Long",
          stack_is(3, SCR_MUSIC, SCR_GENRES) && g_genre_sel == g_long && selected("Long"));
    tap(HL_BTN_MENU, 500);
    CHECK("queue: MENU: the Music menu, cursor on Genres",
          stack_is(2, SCR_MENU, SCR_MUSIC) && g_music_sel == MU_GENRES &&
          selected("Genres"));
    tap(HL_BTN_MENU, 500);
    CHECK("queue: MENU: the main menu, cursor on Music",
          g_scr_n == 1 && scr_cur() == SCR_MENU && g_main_sel == MM_MUSIC &&
          selected("Music"));
}

static void main_menu_playing(void)
{
    wheel(MM_COUNT + 3);
    CHECK("main: playing, the wheel clamps at Now Playing",
          g_main_sel == MM_NOWPLAYING && selected("Now Playing"));
    tap(HL_BTN_SELECT, 800);
    CHECK("main: SELECT on Now Playing opens it",
          stack_is(2, SCR_MENU, SCR_NOWPLAYING) && playing_song("Long Two"));
    tap(HL_BTN_MENU, 500);
    CHECK("main: MENU comes back, cursor on Now Playing",
          g_scr_n == 1 && scr_cur() == SCR_MENU && g_main_sel == MM_NOWPLAYING &&
          selected("Now Playing"));
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK_AZ.img DISK_AZ.img.ranges\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/list_input_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);

    CHECK("image: the pre-allocated files' extents are known", load_ranges(argv[2]) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        unlink(disk);
        return 2;
    }
    CHECK("boot: reached the browser", strstr(hl_log(), "core: entering browser") != NULL);

    main_menu_idle();
    music_menu();
    artists();
    songs();
    genres();
    queue();
    main_menu_playing();

    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) fputs(hl_log(), stderr);
    printf("list-input: done at fake time %.1f s\n", (double)s.now_us / 1e6);
    unlink(disk);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}
