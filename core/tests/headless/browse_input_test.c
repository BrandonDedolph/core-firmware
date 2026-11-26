/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/browse_input_test.c — the album browser's, Playlists', a
 * playlist's and On-The-Go's input: the state each one resets or keeps, which
 * the walks that reach them never look at.
 *
 * otg-walk, search-walk and firmware-walk open these screens and act on them
 * once each, in one direction, with no partial wheel motion and no second
 * press after a scroll. So a confirm prompt that survived a scroll or a
 * re-entry (the next SELECT then clears or deletes a list with no question),
 * an On-The-Go cursor that opened on the wrong row, a free-slot verdict
 * left from the last visit, or the browser's two depths sharing a wheel
 * remainder would all pass. This drives the real run_ui input switch on the
 * --library az disk, as list-input does, and checks the firmware's own state
 * (read while it is frozen) and the panel:
 *
 *   browser    a sub-row wheel remainder on the album list survives a trip
 *              into an album and back (it is the album list's), and one left
 *              in the tracklist does not leak into it; MENU from the
 *              tracklist is the album list, cursor still on the album
 *   Search     an album opened from Search: MENU is the results, not an album
 *              list that was never shown; a later album opened from the
 *              album list goes back to the album list
 *   On-The-Go  opens on its first song (not an action row) when the list has
 *              songs, on Clear when it has none; "Clear? Select again" goes
 *              away on a scroll and is gone when the screen is entered again,
 *              so the next SELECT asks instead of clearing; the slot Save is
 *              offered is decided afresh on every entry (1, then 2 once a
 *              Save has used slot 1)
 *   playlist   a saved slot's "Delete? Select again" goes away on a scroll, so
 *              the next SELECT asks instead of erasing the slot
 *
 * Usage: browse_input_test DISK_AZ.img DISK_AZ.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "app/nav.h"                /* scr_cur, g_scr, g_scr_n               */
#include "app/lists.h"              /* g_main_sel, g_music_sel, MU_*         */
#include "app/library.h"            /* g_albums                              */
#include "app/albums.h"             /* g_albumview, g_br_*, g_det_*, depth   */
#include "app/otg.h"                /* g_otg, g_otg_sel, OTG_ROW_*           */
#include "app/playlists.h"          /* g_pl_sel, g_plt_sel, g_playlists      */
#include "app/search_glue.h"        /* g_search                              */
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xfail_ctx T = { "browse-input", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* ---- helpers (as list_input_test.c's) ------------------------------------- */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int in_list(const char *s) { return hl_find_text_in(hl_panel(), s, 40, HL_H, NULL); }
static int header(const char *s) { return hl_find_text_in(hl_panel(), s, 0, 40, NULL); }
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
/* `rows` detents, at rest (one row per 150 ms), then let the screen settle. */
static void wheel(int rows)
{
    if (rows == 0) return;
    hl_scroll(rows);
    hl_run_ms((uint32_t)(rows < 0 ? -rows : rows) * 150u + 400u);
}
/* Less than a row: `clicks` raw steps in one event, a remainder left behind. */
static void nudge(int clicks)
{
    hl_wheel_clicks(clicks);
    hl_run_ms(400);
}
static void tap(unsigned btn, uint32_t settle) { hl_tap(btn, settle); }
/* Past SEL_HOLD_US: the add-to-On-The-Go gesture. */
static void hold_select(void)
{
    hl_press(HL_BTN_SELECT, 700);
    hl_run_ms(1800);
}
static int pred_header(void *a) { return header((const char *)a); }
static int log_has_since(size_t from, const char *s)
{
    return from <= hl_log_len() && strstr(hl_log() + from, s) != NULL;
}

static int top_is(screen_t below, screen_t top)
{
    return g_scr_n >= 2 && scr_cur() == top && g_scr[g_scr_n - 2] == below;
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

/* The unfiltered album list's row showing the album whose folder names `s`. */
static int album_row(const char *s)
{
    for (int i = 0; i < g_albumview_n; i++) {
        if (strstr(g_albums[g_albumview[i]].folder, s)) return i;
    }
    return -1;
}

/* From the Music menu (cursor anywhere): Albums, the cursor on `album`. */
static int to_album_row(const char *album)
{
    wheel(MU_ALBUMS - g_music_sel);
    tap(HL_BTN_SELECT, 800);
    if (!top_is(SCR_MUSIC, SCR_BROWSER) || g_dir_depth != 0) return -1;
    int k = album_row(album);
    if (k < 0) return -1;
    wheel(k - g_br_sel);
    return g_br_sel == k ? k : -1;
}

/* ---- the album browser ----------------------------------------------------- */

static void browser_accumulators(void)
{
    int k = to_album_row("Alphabet Soup");
    CHECK("browser: Albums, the cursor on Alphabet Soup (48 tracks)", k >= 0 &&
          selected("Alphabet Soup"));

    nudge(2);                               /* half a row on the album list */
    CHECK("browser: half a row does not move the album cursor", g_br_sel == k);
    tap(HL_BTN_SELECT, 1500);
    CHECK("browser: SELECT enters the album, its first track",
          scr_cur() == SCR_BROWSER && g_dir_depth == 1 && g_browse_n == 48 &&
          g_det_sel == 0);

    /* The tracklist starts from no remainder: half a row stays half a row
     * (with the album list's half carried in, it would be a whole one). */
    nudge(2);
    CHECK("browser: half a row in the tracklist does not move it", g_det_sel == 0);
    nudge(2);
    CHECK("browser: ...a second half is the next track", g_det_sel == 1);
    nudge(2);                               /* the tracklist's own half */
    CHECK("browser: ...and a third half does not move it", g_det_sel == 1);

    tap(HL_BTN_MENU, 800);
    CHECK("browser: MENU from the tracklist is the album list, cursor on the album",
          top_is(SCR_MUSIC, SCR_BROWSER) && g_dir_depth == 0 && g_br_sel == k &&
          selected("Alphabet Soup"));
    /* The album list's half from before the album was opened is still there;
     * the tracklist's half is not added to it. */
    nudge(2);
    CHECK("browser: the album list's half row is kept across the album: "
          "another half moves it one row", g_br_sel == k + 1);
    nudge(2);
    CHECK("browser: ...and the tracklist's half was not carried out of it",
          g_br_sel == k + 1);
    tap(HL_BTN_MENU, 600);
    CHECK("browser: MENU: the Music menu, cursor on Albums",
          top_is(SCR_MENU, SCR_MUSIC) && g_music_sel == MU_ALBUMS);
}

/* The ring: A-Z, 0-9, SPACE, DEL, DONE (ui/search.h); one row a cell. */
#define RING_N    39
#define CELL_DONE 38

static void ring_to(int cell)
{
    int d = (cell - (int)g_search.cell + RING_N) % RING_N;
    if (d > RING_N / 2) d -= RING_N;
    wheel(d);
}

static void search_back(void)
{
    wheel(MU_SEARCH - g_music_sel);
    tap(HL_BTN_SELECT, 500);
    CHECK("search: Music > Search", hl_run_until(pred_header, "Search", 100, 3000));
    for (const char *c = "LO"; *c; c++) {
        ring_to(*c - 'A');
        tap(HL_BTN_SELECT, 500);
    }
    ring_to(CELL_DONE);
    tap(HL_BTN_SELECT, 500);
    int want = -1;
    for (int i = 0; i < g_search.nhit; i++) {
        if (g_search.hit[i].type == SEARCH_T_ALBUM &&
            strstr(g_albums[g_search.hit[i].idx].folder, "Long Player")) {
            want = i;
        }
    }
    CHECK("search: \"LO\" finds the album Long Player", want >= 0);
    wheel(want - g_search.sel);
    CHECK("search: the cursor on it", g_search.sel == want && selected("Long Player"));
    tap(HL_BTN_SELECT, 1500);
    CHECK("search: SELECT opens its tracklist above Search",
          top_is(SCR_SEARCH, SCR_BROWSER) && g_dir_depth == 1 && g_browse_n == 2);
    tap(HL_BTN_MENU, 600);
    CHECK("search: MENU is the results, not an album list",
          scr_cur() == SCR_SEARCH && g_dir_depth == 0 && header("Search") &&
          selected("Long Player"));
    /* MENU on the results is the ring, then the Music menu. */
    for (int i = 0; i < 3 && scr_cur() == SCR_SEARCH; i++) tap(HL_BTN_MENU, 600);
    CHECK("search: MENU out of Search: the Music menu",
          top_is(SCR_MENU, SCR_MUSIC) && g_music_sel == MU_SEARCH);

    /* The same album, now from the album list: back is the album list. */
    int k = to_album_row("Long Player");
    CHECK("search: Albums, the cursor on Long Player", k >= 0);
    tap(HL_BTN_SELECT, 1500);
    CHECK("search: ...entered", g_dir_depth == 1 && g_browse_n == 2);
    tap(HL_BTN_MENU, 600);
    CHECK("search: MENU from an album the album list opened is the album list",
          top_is(SCR_MUSIC, SCR_BROWSER) && g_dir_depth == 0 && g_br_sel == k &&
          selected("Long Player"));
}

/* ---- Playlists, On-The-Go, a saved playlist -------------------------------- */

static void to_playlists(void)
{
    for (int i = 0; i < 6 && g_scr_n > 1; i++) tap(HL_BTN_MENU, 400);
    tap(HL_BTN_SELECT, 500);                          /* main menu -> Music */
    wheel(MU_PLAYLISTS - g_music_sel);
    tap(HL_BTN_SELECT, 300);
    CHECK("nav: Music > Playlists", hl_run_until(pred_header, "Playlists", 100, 5000) &&
          scr_cur() == SCR_PLAYLISTS);
}
static int open_otg(void)
{
    wheel(-g_pl_sel);
    tap(HL_BTN_SELECT, 300);
    return hl_run_until(pred_header, "On-The-Go", 100, 5000) && scr_cur() == SCR_OTG;
}

static void otg(void)
{
    /* Three songs on the live list: Beethoven Bits, held on its album row. */
    tap(HL_BTN_SELECT, 500);                          /* main menu -> Music */
    CHECK("otg: the Beethoven Bits album row", to_album_row("Beethoven Bits") >= 0);
    hold_select();
    CHECK("otg: holding SELECT adds its three songs", g_otg.n == 3);

    to_playlists();
    CHECK("otg: Playlists opens on On-The-Go", g_pl_sel == 0);
    CHECK("otg: SELECT opens On-The-Go", open_otg());
    CHECK("otg: ...on its first song, not an action row",
          g_otg_sel == OTG_ROW_FIRST && selected("Allegro"));
    CHECK("otg: ...Save offers slot 1 (all five free)", g_otg_free_slot == 1);

    wheel(-2);
    CHECK("otg: up to Clear Playlist", g_otg_sel == OTG_ROW_CLEAR);
    tap(HL_BTN_SELECT, 300);
    CHECK("otg: SELECT on Clear asks first", in_list("Clear? Select again") &&
          g_otg.n == 3);
    wheel(1);
    CHECK("otg: moving off the row takes the question away",
          g_otg_confirm.armed == 0 && in_list("Clear Playlist") &&
          !in_list("Clear? Select again"));
    wheel(-1);
    tap(HL_BTN_SELECT, 300);
    CHECK("otg: ...so SELECT back on Clear asks again (the list is not cleared)",
          g_otg.n == 3 && in_list("Clear? Select again"));

    /* Leave with the question up and come straight back: it is not. */
    tap(HL_BTN_MENU, 300);
    CHECK("otg: MENU is Playlists", scr_cur() == SCR_PLAYLISTS);
    CHECK("otg: SELECT opens On-The-Go again", open_otg());
    CHECK("otg: ...without the question", g_otg_confirm.armed == 0 &&
          in_list("Clear Playlist") && !in_list("Clear? Select again"));
    CHECK("otg: ...on its first song again (not where it was left)",
          g_otg_sel == OTG_ROW_FIRST && selected("Allegro"));
    CHECK("otg: ...the list intact", g_otg.n == 3);

    /* Save into slot 1; the next visit offers slot 2. */
    wheel(-1);
    CHECK("otg: up to Save Playlist", g_otg_sel == OTG_ROW_SAVE);
    size_t mark = hl_log_len();
    tap(HL_BTN_SELECT, 2000);
    CHECK("otg: Save writes slot 1", log_has_since(mark, "core: otg slot 1 lba ") &&
          log_has_since(mark, "core: otg slot save rc 00000000 wrote 3 of 3"));
    CHECK("otg: ...and returns to Playlists, the saved list selected",
          scr_cur() == SCR_PLAYLISTS && selected("On-The-Go 1") && g_otg.n == 0);
    CHECK("otg: SELECT on On-The-Go, now empty", open_otg());
    CHECK("otg: ...Save would use slot 2 now", g_otg_free_slot == 2);
    CHECK("otg: ...an empty list opens on Clear", g_otg_sel == OTG_ROW_CLEAR);
    tap(HL_BTN_MENU, 500);
}

static void playlist_delete(void)
{
    int k = -1;
    for (int i = 0; i < g_playlists_n; i++) {
        if (strcmp(g_playlists[i].name, "On-The-Go 1") == 0) k = i + 1;
    }
    CHECK("playlist: On-The-Go 1 is listed", k > 0 && scr_cur() == SCR_PLAYLISTS);
    wheel(k - g_pl_sel);
    CHECK("playlist: the cursor on On-The-Go 1", g_pl_sel == k && selected("On-The-Go 1"));
    tap(HL_BTN_SELECT, 1000);
    CHECK("playlist: SELECT opens the slot: three tracks and Delete",
          scr_cur() == SCR_PLAYLIST && g_pl_tracks_n == 3 && g_pl_slot == 1);
    wheel(4);
    CHECK("playlist: down to Delete Playlist", g_plt_sel == 3 && selected("Delete Playlist"));
    tap(HL_BTN_SELECT, 300);
    CHECK("playlist: SELECT on Delete asks first", in_list("Delete? Select again"));
    wheel(-1);
    CHECK("playlist: moving off the row takes the question away",
          g_pl_confirm.armed == 0 && !in_list("Delete? Select again"));
    wheel(1);
    size_t mark = hl_log_len();
    tap(HL_BTN_SELECT, 300);
    CHECK("playlist: ...so SELECT back on Delete asks again (nothing erased)",
          scr_cur() == SCR_PLAYLIST && in_list("Delete? Select again") &&
          !log_has_since(mark, "core: otg slot erase"));
    tap(HL_BTN_SELECT, 2000);
    CHECK("playlist: SELECT on the question erases the slot",
          log_has_since(mark, "core: otg slot erase rc 00000000") &&
          scr_cur() == SCR_PLAYLISTS && !in_list("On-The-Go 1"));
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK_AZ.img DISK_AZ.img.ranges\n", argv[0]);
        return 2;
    }
    char disk[] = "/tmp/browse_input_XXXXXX";
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

    tap(HL_BTN_SELECT, 500);                          /* main menu -> Music */
    CHECK("boot: the Music menu", top_is(SCR_MENU, SCR_MUSIC));
    browser_accumulators();
    search_back();
    for (int i = 0; i < 6 && g_scr_n > 1; i++) tap(HL_BTN_MENU, 400);
    otg();
    playlist_delete();

    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) fputs(hl_log(), stderr);
    printf("browse-input: done at fake time %.1f s\n", (double)s.now_us / 1e6);
    unlink(disk);
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}
