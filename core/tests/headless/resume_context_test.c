/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/resume_context_test.c — resume brings back the QUEUE, not
 * just the track.
 *
 * resume_capture() saves the playing track, its position, and what it is
 * playing IN: the queue kind the builder recorded, the queue index, the
 * Shuffle Songs seed, the playlist's name hash and the player's shuffle deal
 * (order seed + pinned index). resume_restore() rebuilds that queue at boot,
 * deals the saved order back over it, and puts the main-menu cursor on Now
 * Playing. firmware-walk's second power-on only resumes an album track, so a
 * lost kind or a lost deal falls back to (or happens to equal) the album and
 * nothing notices. This walks every queue kind the restore knows, each across
 * its own power cut, on one disk, with Shuffle (Songs) on throughout:
 *
 *   session 0   Settings > Playback > Shuffle: Songs; then play ALBUM
 *   session 1   verify album;    play SONGS      (Music > Songs, 7 tracks)
 *   session 2   verify songs;    play ARTIST     (Artists > Test Tones > All Songs)
 *   session 3   verify artist;   play GENRE      (Genres > Test, 7 tracks)
 *   session 4   verify genre;    play SHUFFLE    (Music > Shuffle Songs)
 *   session 5   verify shuffle;  play PLAYLIST   (Playlists > Mixed, 5 tracks)
 *   session 6   verify playlist; play ON-THE-GO  (a 4-entry live list)
 *   session 7   verify on-the-go
 *
 * "play": start the queue on a long track (Long One, or for Shuffle Songs the
 * first long track the draw reaches), pause, press Next and record the track
 * it gives — the track the deal puts after this one — and Prev back to it;
 * then play ~15 s, pause, and let the settings record reach the disk ("cfg
 * save rc 0": the debounce if the drive turns, else a folder read spins it
 * up and the pending write lands). Then a hard power cut: the process just
 * exits.
 *
 * "verify", after the power-on: the main-menu cursor is on Now Playing, the
 * same track is loaded and paused at the same second (+-1 s), in a queue of
 * the same length at the same index, and Next from Now Playing gives the same
 * track it gave before the cut. The last is what a lost queue kind (the album
 * comes back instead: a different length) or a lost shuffle deal (a fresh one
 * from the boot's timer: a different successor) breaks.
 *
 * Throughout: no write outside the pre-allocated files, no panic.
 *
 * Usage: resume_context_test DISK.img DISK.img.ranges
 */

#include "headless.h"
#include "hl_screen.h"
#include "player/player.h"          /* read-only, while the firmware is frozen */
#include "app/lists.h"              /* g_main_sel, MM_NOWPLAYING */
#include "../xfail.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "resume-context", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* What one session left playing, for the next one to find. */
typedef struct {
    int      valid;
    char     kind[16];
    char     track[96];
    char     next[96];
    unsigned secs;
    int      qlen, qidx;
} rec_t;

/* ---- helpers (as walk_test.c's) ------------------------------------------ */

static int lum(uint16_t c)
{
    return ((c >> 11) & 0x1F) * 2 + ((c >> 5) & 0x3F) + (c & 0x1F) * 2;
}
static int on_screen(const char *s) { return hl_find_text(hl_panel(), s, NULL); }
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
static void hold_select(void)       /* past SEL_HOLD_US: add to On-The-Go */
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
static int pred_header(void *a)
{
    return hl_find_text_in(hl_panel(), (const char *)a, 0, 40, NULL);
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

/* The loaded track's name ("" when nothing is), copied out. */
static void track_name(char *out, size_t n)
{
    const char *nm = player_active() ? player_track_name() : NULL;
    snprintf(out, n, "%s", nm ? nm : "");
}
static int track_is(const char *want)
{
    char cur[96];
    track_name(cur, sizeof cur);
    return want[0] != '\0' && strcmp(cur, want) == 0;
}

/* The paused position, in seconds; ~0u when not paused. Only while paused:
 * a PLAYING player's clock reads USEC_TIMER, and a timer read from the test
 * thread moves fake time and takes the baton from under the firmware. */
static unsigned paused_at_s(void)
{
    return (player_active() && player_paused()) ? player_elapsed_s() : ~0u;
}

/* hl_run_until predicate: a track other than `a` is loaded. */
static int pred_moved(void *a)
{
    char cur[96];
    track_name(cur, sizeof cur);
    return cur[0] != '\0' && strcmp(cur, (const char *)a) != 0;
}

static void boot(const char *disk, const char *ranges)
{
    CHECK("image: the pre-allocated files' extents are known", load_ranges(ranges) >= 3);
    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        _exit(2);
    }
    CHECK("boot: reached the browser", log_has("core: entering browser"));
}

static int g_report_fd = -1;

/* A hard power cut: the firmware thread is frozen mid-wait and simply stops
 * existing. The record goes to the parent first. */
static _Noreturn void power_cut(const char *what, const rec_t *r)
{
    hl_stats_t s = hl_stats();
    CHECK("disk: every write inside a pre-allocated file", s.bad_writes == 0);
    CHECK("never panicked", hl_parked() != HL_PARK_PANIC);
    if (T.fails && getenv("HL_DUMP_LOG")) {
        fputs(hl_log(), stderr);
    }
    printf("%s: power cut at fake time %.1f s\n", what, (double)s.now_us / 1e6);
    if (r && g_report_fd >= 0) {
        (void)!write(g_report_fd, r, sizeof *r);
    }
    int rc = xfail_done(&T);
    fflush(NULL);
    _exit(rc);
}

static void to_main_menu(void)
{
    for (int i = 0; i < 6; i++) tap(HL_BTN_MENU);
}

/* Main menu -> Music -> `row`, entered. */
static int music(const char *row)
{
    to_main_menu();
    int ok = select_row("Music");
    tap(HL_BTN_SELECT);
    ok = ok && select_row(row);
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);               /* a folder read; maybe a spin-up */
    return ok;
}

/* On a list: SELECT (a tap) on `row` plays it; Now Playing comes up. */
static int play_row(const char *row)
{
    int ok = select_row(row);
    hl_tap(HL_BTN_SELECT, 1500);
    return ok && player_active() && on_screen("Now Playing");
}

/*
 * On Now Playing, the track just started: record the queue around it and the
 * track Next gives, come back to it, play ~15 s, pause, and wait until the
 * settings record holding the paused position is on the disk.
 */
static void record_and_pause(rec_t *r, const char *kind)
{
    char tag[64];
    memset(r, 0, sizeof *r);
    snprintf(r->kind, sizeof r->kind, "%s", kind);

    tap(HL_BTN_PLAY);                                   /* pause */
    snprintf(tag, sizeof tag, "%s: playing, then paused", kind);
    CHECK(tag, player_active() && player_paused());
    track_name(r->track, sizeof r->track);
    r->qlen = player_queue_len();
    r->qidx = player_queue_current();

    tap(HL_BTN_RIGHT);                                  /* Next */
    track_name(r->next, sizeof r->next);
    snprintf(tag, sizeof tag, "%s: Next moves to another track", kind);
    CHECK(tag, player_active() && r->next[0] && strcmp(r->next, r->track) != 0);
    tap(HL_BTN_LEFT);                                   /* Prev: back (at 0:00) */
    snprintf(tag, sizeof tag, "%s: Prev comes back to it", kind);
    CHECK(tag, track_is(r->track) && player_queue_current() == r->qidx);

    tap(HL_BTN_PLAY);                                   /* play on */
    hl_run_ms(15000);
    size_t mark = hl_log_len();
    tap(HL_BTN_PLAY);                                   /* pause mid-track */
    r->secs = paused_at_s();
    snprintf(tag, sizeof tag, "%s: paused mid-track (>= 10 s in)", kind);
    CHECK(tag, player_paused() && track_is(r->track) &&
               r->secs >= 10 && r->secs < 90);

    /* The capture marks the record dirty; it is written SOFT once the 3 s
     * debounce runs out — at once if the platters turn, otherwise at the
     * next spin-up. Long One is twice the anti-skip buffer, so the player
     * may have parked the drive: a folder read (Albums > Long Player,
     * nothing played) spins it, as a user browsing would. */
    log_wait_t w = { mark, "core: cfg save rc 00000000 seq " };
    int saved = hl_run_until(pred_log, &w, 250, 4000);
    if (!saved) {
        saved = music("Albums") && select_row("Long Player") &&
                (tap(HL_BTN_SELECT), hl_run_until(pred_log, &w, 250, 10000));
    }
    snprintf(tag, sizeof tag, "%s: the paused position reaches the disk", kind);
    CHECK(tag, saved && player_paused() && track_is(r->track));
    r->valid = 1;
    printf("%s: '%s' at %u s, queue %d/%d, next '%s'\n", kind, r->track,
           r->secs, r->qidx, r->qlen, r->next);
}

/* After a power-on: the session before this one is back, whole. */
static void verify(const rec_t *r)
{
    char tag[96];
#define VCHECK(what, cond) \
    (snprintf(tag, sizeof tag, "%s resumed: %s", r->kind, (what)), CHECK(tag, (cond)))

    CHECK("previous session recorded its queue", r->valid);
    if (!r->valid) return;
    VCHECK("the main-menu cursor is on Now Playing",
           g_main_sel == MM_NOWPLAYING && selected("Now Playing"));
    VCHECK("the same track is loaded", track_is(r->track));
    VCHECK("...paused", player_paused() && !hl_stats().audio_running);
    unsigned at = paused_at_s();
    VCHECK("...at the same second (+-1 s)", at + 1 >= r->secs && at <= r->secs + 1);
    VCHECK("the same queue length", player_queue_len() == r->qlen);
    VCHECK("...at the same queue index", player_queue_current() == r->qidx);

    CHECK("select Now Playing", select_row("Now Playing"));
    tap(HL_BTN_SELECT);
    VCHECK("Now Playing", on_screen("Now Playing") || on_screen("Paused"));
    tap(HL_BTN_RIGHT);
    /* The drive has parked since the boot: the open waits out a spin-up. */
    (void)hl_run_until(pred_moved, (void *)r->track, 100, 5000);
    char got[96];
    track_name(got, sizeof got);
    VCHECK("Next gives the track it gave before the power cut",
           strcmp(got, r->next) == 0);
    if (strcmp(got, r->next) != 0) {
        printf("%s: Next gave '%s', expected '%s'\n", r->kind, got, r->next);
    }
#undef VCHECK
}

/* ---- the sessions ---------------------------------------------------------- */

static rec_t g_prev;      /* what the last session left; inherited by fork */

static _Noreturn void s_album(const char *disk, const char *ranges)
{
    rec_t r;
    T.suite = "resume-context(album)";
    boot(disk, ranges);
    /* Shuffle (Songs) on, for every session after: each queue is dealt. */
    to_main_menu();
    CHECK("setup: Settings > Playback > Shuffle",
          select_row("Settings") && (tap(HL_BTN_SELECT), select_row("Playback")) &&
          (tap(HL_BTN_SELECT), select_row("Shuffle")));
    tap(HL_BTN_SELECT);
    {
        hl_text_hit_t h;
        CHECK("setup: Shuffle reads Songs",
              hl_find_text_in(hl_panel(), "Shuffle", 40, HL_H, &h) &&
              hl_find_text_in(hl_panel(), "Songs", h.y - 8, h.y + h.h + 8, NULL));
    }
    CHECK("album: Music > Albums > Long Player",
          music("Albums") && select_row("Long Player"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    CHECK("album: Long One plays", play_row("Long One"));
    record_and_pause(&r, "album");
    power_cut("album", &r);
}

static _Noreturn void s_songs(const char *disk, const char *ranges)
{
    rec_t r;
    T.suite = "resume-context(songs)";
    boot(disk, ranges);
    verify(&g_prev);
    /* "Songs" is also the tail of the "Shuffle Songs" row under it: land on
     * that one and step up. */
    to_main_menu();
    CHECK("songs: Music > Songs",
          select_row("Music") && (tap(HL_BTN_SELECT), select_row("Shuffle Songs")) &&
          (hl_scroll(-1), hl_run_ms(450), tap(HL_BTN_SELECT), hl_run_ms(1000),
           hl_find_text_in(hl_panel(), "Songs", 0, 40, NULL)));
    CHECK("songs: Long One plays", play_row("Long One"));
    record_and_pause(&r, "songs");
    CHECK("songs: the queue is the whole library", r.qlen == 7);
    power_cut("songs", &r);
}

static _Noreturn void s_artist(const char *disk, const char *ranges)
{
    rec_t r;
    T.suite = "resume-context(artist)";
    boot(disk, ranges);
    verify(&g_prev);
    CHECK("artist: Music > Artists > Test Tones",
          music("Artists") && select_row("Test Tones"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    CHECK("artist: All Songs", select_row("All Songs"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    CHECK("artist: Long One plays", play_row("Long One"));
    record_and_pause(&r, "artist");
    power_cut("artist", &r);
}

static _Noreturn void s_genre(const char *disk, const char *ranges)
{
    rec_t r;
    T.suite = "resume-context(genre)";
    boot(disk, ranges);
    verify(&g_prev);
    CHECK("genre: Music > Genres > Test", music("Genres") && select_row("Test"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    CHECK("genre: Long One plays", play_row("Long One"));
    record_and_pause(&r, "genre");
    CHECK("genre: the queue is the genre's seven songs", r.qlen == 7);
    power_cut("genre", &r);
}

static _Noreturn void s_shuffle(const char *disk, const char *ranges)
{
    rec_t r;
    T.suite = "resume-context(shuffle)";
    boot(disk, ranges);
    verify(&g_prev);
    to_main_menu();
    CHECK("shuffle: Music > Shuffle Songs",
          select_row("Music") && (tap(HL_BTN_SELECT), select_row("Shuffle Songs")));
    hl_tap(HL_BTN_SELECT, 1500);
    CHECK("shuffle: a draw plays", player_active() && on_screen("Now Playing"));
    /* The draw starts wherever the seed put it; the short tracks are over
     * before the 15 s this needs, so step (paused) to the first long one. */
    tap(HL_BTN_PLAY);
    for (int i = 0; i < 7 && !strstr(player_track_name(), "Long"); i++) {
        tap(HL_BTN_RIGHT);
    }
    CHECK("shuffle: on a long track", player_active() &&
          strstr(player_track_name(), "Long") != NULL);
    tap(HL_BTN_PLAY);                       /* record_and_pause pauses it */
    record_and_pause(&r, "shuffle");
    power_cut("shuffle", &r);
}

static _Noreturn void s_playlist(const char *disk, const char *ranges)
{
    rec_t r;
    T.suite = "resume-context(playlist)";
    boot(disk, ranges);
    verify(&g_prev);
    to_main_menu();
    CHECK("playlist: Playlists", select_row("Playlists"));
    tap(HL_BTN_SELECT);
    CHECK("playlist: the Playlists screen",
          hl_run_until(pred_header, "Playlists", 250, 5000));
    CHECK("playlist: Mixed", select_row("Mixed"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1500);
    CHECK("playlist: Long One plays", play_row("Long One"));
    record_and_pause(&r, "playlist");
    CHECK("playlist: the queue is Mixed's five rows", r.qlen == 5);
    power_cut("playlist", &r);
}

static _Noreturn void s_otg(const char *disk, const char *ranges)
{
    rec_t r;
    T.suite = "resume-context(otg)";
    boot(disk, ranges);
    verify(&g_prev);
    /* The live list: the Beethoven Bits album (three), then Long One. */
    CHECK("otg: Music > Albums > Beethoven Bits",
          music("Albums") && select_row("Beethoven Bits"));
    hold_select();
    CHECK("otg: the album is added", on_screen("Added 3 songs"));
    hl_run_ms(1000);
    CHECK("otg: Long Player", select_row("Long Player"));
    tap(HL_BTN_SELECT);
    hl_run_ms(1000);
    CHECK("otg: Long One", select_row("Long One"));
    size_t mark = hl_log_len();
    hold_select();
    log_wait_t w = { mark, "core: otg save rc 00000000 seq " };
    CHECK("otg: the live list is committed to COREOTG.DAT",
          hl_run_until(pred_log, &w, 500, 60000));
    to_main_menu();
    CHECK("otg: Playlists > On-The-Go", select_row("Playlists"));
    tap(HL_BTN_SELECT);
    CHECK("otg: the Playlists screen", hl_run_until(pred_header, "Playlists", 250, 5000));
    CHECK("otg: On-The-Go row", select_row("On-The-Go"));
    tap(HL_BTN_SELECT);
    CHECK("otg: the On-The-Go screen", hl_run_until(pred_header, "On-The-Go", 250, 5000));
    CHECK("otg: Long One plays", play_row("Long One"));
    record_and_pause(&r, "otg");
    CHECK("otg: the queue is the live list's four", r.qlen == 4);
    power_cut("otg", &r);
}

static _Noreturn void s_last(const char *disk, const char *ranges)
{
    T.suite = "resume-context(last)";
    boot(disk, ranges);
    verify(&g_prev);
    power_cut("last", NULL);
}

typedef void (*session_fn)(const char *, const char *);

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s DISK.img DISK.img.ranges\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);       /* progress lines as they happen */
    char disk[] = "/tmp/resume_context_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || copy_disk(argv[1], disk) != 0) {
        fprintf(stderr, "cannot copy %s\n", argv[1]);
        return 2;
    }
    close(fd);

    static const struct { session_fn fn; const char *name; } S[] = {
        { s_album,    "album"    }, { s_songs,    "songs"    },
        { s_artist,   "artist"   }, { s_genre,    "genre"    },
        { s_shuffle,  "shuffle"  }, { s_playlist, "playlist" },
        { s_otg,      "otg"      }, { s_last,     "last"     },
    };
    int ok = 1;
    /* Each session is its own process: the firmware's statics start from
     * .bss again, as on a real power-on, and each boots the disk the one
     * before it wrote. Each reports what it left playing down a pipe. */
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        int pfd[2];
        if (pipe(pfd) != 0) return 2;
        fflush(NULL);
        pid_t p = fork();
        if (p == 0) {
            close(pfd[0]);
            g_report_fd = pfd[1];
            S[i].fn(disk, argv[2]);
            _exit(3);
        }
        close(pfd[1]);
        rec_t r;
        memset(&r, 0, sizeof r);
        if (read(pfd[0], &r, sizeof r) != (ssize_t)sizeof r) {
            memset(&r, 0, sizeof r);
        }
        close(pfd[0]);
        int st = 0;
        waitpid(p, &st, 0);
        g_prev = r;
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            fprintf(stderr, "resume-context: session %s FAILED\n", S[i].name);
            ok = 0;
        }
    }
    unlink(disk);
    return ok ? 0 : 1;
}
