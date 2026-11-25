/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Boot bring-up (proven on real 5.5G hardware): UART banner, 30 MHz clock,
 * unified cache, 100 Hz tick + IRQs, then — if the BCM LCD is powered — the
 * disk/audio stack. From there kernel_main mounts the FAT32 volume and runs the
 * menu UI: a main menu (Music / … / Now Playing), a Music sub-menu, and an
 * album/folder browser that streams the selected track through the background
 * player (core/player/player.c) and out the DMA-fed DAC. MENU pops screens; a
 * playing track keeps going while you navigate.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/ata.h"
#include "hw/clickwheel.h"
#include "hw/backlight.h"
#include "hw/battery.h"
#include "hw/headphone.h"
#include "hw/i2c.h"
#include "hw/power.h"
#include "hw/audio.h"
#include "hw/piezo.h"
#include "hw/rtc.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "cache.h"
#include "console.h"
#include "panic.h"
#include "config.h"
#include "resume_ctx.h"
#include "cfg_commit.h"
#include "evlog.h"
#include "chargestat.h"
#include "idlesleep.h"
#include "drivepower.h"
#include "otg_store.h"
#include "core_version.h"       /* CORE_BUILD_ID: meson vcs_tag, git describe   */
#include "core_version_tag.h"   /* CORE_VERSION:  meson vcs_tag, nearest tag    */
#include "../ui/text.h"
#include "../ui/thumb.h"
#include "../ui/artcache.h"
#include "../ui/screen_charging.h"
#include "../ui/screen_battery.h"
#include "../ui/settings.h"
#include "../ui/eq.h"
#include "../ui/palette.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "../library/idx.h"
#include "../library/sort.h"
#include "../library/playlist.h"
#include "../library/otg.h"
#include "../library/otg_slot.h"
#include "../ui/wheel.h"
#include "../ui/letterindex.h"
#include "../ui/search.h"
#include "../ui/keyhold.h"
#include "../ui/gesture.h"
#include "../ui/sleeptimer.h"
#include "../ui/jackwatch.h"
#include "../ui/settime.h"
#include "datetime.h"
#include "timesync.h"
#include "wallclock.h"
#include "hw/volume.h"
#include "../app/state.h"
#include "../app/layout.h"
#include "../app/util.h"
#include "../app/bootscreen.h"
#include "../app/marquee.h"
#include "../app/clockui.h"
#include "../app/persist.h"
#include "../app/nav.h"
#include "../app/library.h"
#include "../app/battery_ui.h"
#include "../app/playctx.h"
#include "../app/statusbar.h"
#include "../app/playlists.h"
#include "../app/main_internal.h"

/*
 * The Settings shuffle row and the player's playback order are the same three
 * values, deliberately: the byte in the config record is the enum, and
 * settings_apply() hands it straight to player_set_shuffle(). They are
 * declared in two headers because the player must not depend on the UI, so
 * this is the seam that holds them together.
 */
_Static_assert((int)SHUFFLE_OFF    == PLAYER_SHUFFLE_OFF,
               "settings shuffle ids must be the player's");
_Static_assert((int)SHUFFLE_SONGS  == PLAYER_SHUFFLE_SONGS,
               "settings shuffle ids must be the player's");
_Static_assert((int)SHUFFLE_ALBUMS == PLAYER_SHUFFLE_ALBUMS,
               "settings shuffle ids must be the player's");

/* Host-findable version stamp: `core info` scans the OSOS body for this tag
 * (see core/docs/design/companion-app-plan.md, S8). Never printed — not on the
 * UART (the clicky boot golden matches those lines exactly) and not on the
 * screen; the only reader is the host, which greps the image for
 * "CORE-FW-VERSION:". `used` stops the compiler dropping an unreferenced
 * definition; the LINK also needs boot/linker.ld's KEEP on this section,
 * because --gc-sections collects it otherwise (verified: without the KEEP the
 * string is absent from core.bin).
 * tests/scripts/check_version_marker.sh asserts exactly one copy
 * survives into build-hw/core.bin and that its version half is the repo's
 * nearest tag. CORE_BUILD_ID carries "-dirty" when the tree is dirty, which
 * is the point: an image built from an uncommitted tree says so. */
const char core_version_marker[] __attribute__((used, section(".rodata.core_version"))) =
    "CORE-FW-VERSION:" CORE_VERSION "|" CORE_BUILD_ID "\0";


/* ---------------------------------------------------------------------------
 * File / album browser
 *
 * browse_entry_t + BROWSE_MAX/NAME_MAX are shared with the player (player.h):
 * a folder's worth of entries is copied into the player as its queue.
 * ------------------------------------------------------------------------- */

static browse_entry_t g_browse[BROWSE_MAX];
static int            g_browse_n;

/* Browser navigation. The album LIST (depth 0) is index-driven — see g_albums
 * below — while the TRACKLIST (depth 1) is a live folder read into g_browse.
 * Albums are flat on disk (import flattens multi-disc), so the browser is only
 * ever two levels: g_dir_depth is 0 (albums) or 1 (an album's tracks).
 * g_cur_dir is the cluster of the album whose tracks are currently listed. */
static uint32_t g_cur_dir;
int      g_dir_depth;

/* Browser view state (was local to the old browse loop). The album LIST (depth
 * 0) and a single album's TRACKLIST (depth 1) keep SEPARATE selections, so
 * backing out of an album returns the cursor to that album in the list rather
 * than jumping to the top. */
static int g_br_sel, g_br_accum;      /* album list (depth 0)  */
static int g_chip_last_sel, g_chip_dir = 1;  /* cover look-ahead's direction */
/* This tracklist was opened straight from a Search hit, so the album list
 * underneath it was never on screen: MENU must go back to the results, not
 * reveal a list the user did not ask for. Cleared by every other push. */
static int g_br_from_search;

/* Music > Search's whole state, one instance. Up here rather than with the
 * rest of the screen because play_tap_start's policy switch reads it, and
 * that sits above the loaders the Search section is built on. */
static search_t g_search;
static int g_det_sel, g_det_accum;    /* tracklist   (depth 1) */

/* Album art (folder.art) location captured while enumerating the current
 * folder; handed to player_play_queue so the player owns/validates it. */
static uint32_t g_art_clus, g_art_size;

/* When set, the album list (depth 0) only shows folders whose "Artist - Album"
 * name has this artist prefix — the Artists → one-artist's-albums drill-down.
 * Empty means the plain "Albums" list (every folder). */
static char g_artist_filter[NAME_MAX + 1];

/* fat32_readdir callback: collect music subdirectories + playable files into
 * g_browse. Directories named like iPod/OS system folders (or dotfolders) are
 * junk-filtered out; non-playable files are skipped. */
static int browse_collect(void *ud, const fat32_dirent_t *e)
{
    (void)ud;

    /* Capture the folder's album-art sidecar (not shown as a list row). */
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_art_clus = e->first_clus;
        g_art_size = e->size;
        return 0;
    }

    if (g_browse_n >= BROWSE_MAX) return 1;   /* array full: stop enumeration */

    if (e->is_dir) {
        if (is_junk_dir(e->name)) return 0;       /* skip system/Apple folders */
        /* The browser is exactly two levels (albums, then one album's tracks —
         * import flattens multi-disc), so a folder nested INSIDE an album
         * ("Scans", "Artwork") is never navigable. Listing it anyway put a dead
         * entry in g_browse: the wheel ranged over g_browse_n (dirs included)
         * while the tracklist view skipped dirs, so one wheel position selected
         * nothing visible and SELECT there played a different track. */
        if (g_dir_depth != 0) return 0;
        /* Artists drill-down: at the album-list level, keep only this artist's
         * folders (name "Artist - Album"). */
        if (g_dir_depth == 0 && g_artist_filter[0]) {
            char a[NAME_MAX + 1], b2[NAME_MAX + 1];
            split_artist_album(e->name, a, b2);
            if (!name_eq_ci(a, g_artist_filter)) return 0;
        }
        browse_entry_t *b = &g_browse[g_browse_n++];
        copy_display_name(b->name, e->name, 0);   /* keep folder name as-is */
        b->clus   = e->first_clus;
        b->size   = 0;
        b->fmt    = 0;
        b->is_dir = 1;
        /* A row is never memset, so every field has to be written or it keeps
         * whatever the previous listing left there. A folder never plays, so
         * it belongs to no album and has no place in one. */
        b->album     = 0;
        b->order_key = 0xFFFFFFFFu;
        return 0;
    }

    /* The album/artist lists (depth 0) show only folders — hide loose files at
     * the root (leftover TEST.*, stray downloads) so they don't clutter it. */
    if (g_dir_depth == 0) return 0;

    int fmt = classify_ext(e->name);
    if (fmt < 0) return 0;
    browse_entry_t *b = &g_browse[g_browse_n++];
    copy_display_name(b->name, e->name, 1);       /* trim ".flac" -> title */
    b->clus   = e->first_clus;
    b->size   = e->size;
    b->fmt    = (uint8_t)fmt;
    b->is_dir = 0;
    b->art_clus = b->art_size = 0;                 /* album play: queue-level art */
    /* One folder is one album, so every row here shares a group id. The place
     * within it is the index's and browse_bind() writes it; "unbound" is the
     * right value until then, and a listing whose directory read failed never
     * gets that far. */
    b->album     = 1;
    b->order_key = 0xFFFFFFFFu;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Design-matched list chrome (menus.jsx): status strip, header, rows, scrollbar
 * ------------------------------------------------------------------------- */

/*
 * Now-Playing scrub (seek) state.
 *
 * The decoders and the player have supported seeking all along
 * (player_seek_to); nothing was bound to it, which made long tracks,
 * podcasts and audiobooks effectively unusable.
 *
 * The wheel aims at a target rather than seeking on every detent: a seek stops
 * the DAC, re-primes the ring and on a backward jump rewinds the file, so doing
 * that per detent would stutter and hammer the disk. The target is committed
 * once the wheel goes quiet (SCRUB_COMMIT_US), which is also what makes a long
 * drag across a track cost exactly one seek.
 */
#define SCRUB_COMMIT_US  400000u     /* wheel quiet this long -> perform seek  */
#define SCRUB_EXIT_US   4000000u     /* ...and this long -> back to volume     */
#define SCRUB_STEP_S          5      /* seconds per detent at rest             */

static int      g_np_scrub;          /* 1 = wheel seeks instead of volume      */
static uint32_t g_scrub_target_s;    /* where the wheel is currently aiming    */
static uint32_t g_scrub_last_us;     /* last detent, for commit/exit timing    */
static int      g_scrub_dirty;       /* target moved since the last commit     */

/* SELECT press-length arbitration on Now Playing: tap = scrubber, hold = queue. */
#define SEL_HOLD_US 450000u
/* PLAY held this long sleeps the device; released sooner, it is play/pause. */
#define PLAY_HOLD_US 2000000u

static uint32_t g_sel_down_us;
int      g_sel_pending;

int np_scrubbing(void)
{
    return g_np_scrub;
}

static void scrub_enter(void)
{
    g_np_scrub       = 1;
    g_scrub_target_s = player_elapsed_s();
    g_scrub_last_us  = mmio_read32(USEC_TIMER_ADDR);
    g_scrub_dirty    = 0;
}

void scrub_exit(void)
{
    g_np_scrub    = 0;
    g_scrub_dirty = 0;
}

/*
 * Hold RIGHT / LEFT on the player screens: fast forward and rewind. The same
 * aim-then-commit trade the wheel scrubber makes above, for the same reason —
 * a seek per tick would stop the DAC, re-prime the ring and rewind the file
 * four times a second. ui/gesture.c owns the press-length decision, the ramp
 * and the clamps; run_ui() only feeds them and acts on what comes back.
 *
 * File-scope rather than run_ui() locals because the transport band renderer
 * (well above run_ui) has to know where the hold is aiming.
 */
static seekhold_t g_ff = { .dir = +1 };
static seekhold_t g_rw = { .dir = -1 };

/*
 * Where the transport band should point: the wheel scrubber's target, a
 * RIGHT/LEFT hold's, or nothing at all — in which case the band shows the
 * live position. One question, asked once, so the renderer stays ignorant of
 * which gesture is aiming.
 */
static int np_aim_target(uint32_t *target)
{
    if (np_scrubbing())         { *target = g_scrub_target_s;      return 1; }
    if (seekhold_active(&g_ff)) { *target = seekhold_target(&g_ff); return 1; }
    if (seekhold_active(&g_rw)) { *target = seekhold_target(&g_rw); return 1; }
    return 0;
}

#define LOCK_FLASH_US 1000000u

/* Set by enter_standby() when the PMU refused the power-down and the device
 * was brought back instead: run_ui reads and clears it to resync its
 * backlight/idle state with the screen enter_standby already relit. */
static int g_standby_refused;

/* settings_commit() modes — CFG_COMMIT_IDLE / _SOFT / _FORCE / _LAST — and
 * the policy behind them (the DISKSAFE exemption in particular, which is not
 * optional, and the rule that only a FORCE ever wakes a parked drive) live in
 * cfg_commit.h with the gate that implements them. */

/* Measured cost of the last full-frame present; defined with the present
 * throttle further down. Reported on the stats line below so the number the
 * repaint throttle has always been guessing at becomes observable. */
uint32_t g_present_cost_us;

/*
 * The jack probe's narration. These lines go through the evlog tap into
 * CORELOG.BIN, so a bench session on a cable-less device leaves a written
 * record: plug, unplug, dump the log afterwards. Budgeted because the RAW
 * level is un-debounced and a cable chewed in a pocket would otherwise fill
 * the ring — the same "cannot spam" promise the UART probe makes. Debounced
 * edges are unbudgeted: at most one per genuine transition.
 */
#define JACK_RAW_LINE_BUDGET 64u

static unsigned g_jack_raw_lines;

static void jack_narrate_raw(int raw, unsigned edges)
{
    if (g_jack_raw_lines >= JACK_RAW_LINE_BUDGET) {
        return;
    }
    if (++g_jack_raw_lines == JACK_RAW_LINE_BUDGET) {
        uart_puts("core: jack raw line budget spent; silent from here\n");
        return;
    }
    uart_puts("core: jack raw=");
    uart_dec(raw);
    uart_puts(" n=");
    uart_dec((int)edges);
    uart_putc('\n');
}

/* The About screen's jack token carries the pin configuration, and ui/ is
 * host-built so it cannot see hal/hw/headphone.h. Two names for one bit is
 * fine as long as they cannot drift. */
_Static_assert(ABOUT_JACK_PIN_ENABLED == HEADPHONE_PIN_ENABLED &&
               ABOUT_JACK_PIN_OUTPUT  == HEADPHONE_PIN_OUTPUT,
               "ui/settings.h mirrors hal/hw/headphone.h's pin-config bits");

/* Slim right-edge scrollbar (menus.jsx Scrollbar); no-op when everything fits.
 * `y0` is the list origin (differs between the full list and the detail view). */
/* ---------------------------------------------------------------------------
 * Album detail view (collection-detail.jsx AlbumDetail): a 56x56 art hero with
 * title + track count, then the folder's tracklist. Shown when you enter an
 * album folder (browser depth > 0). The hero art is the entered folder's
 * folder.art, downscaled on-device to 56x56 (thumb_downscale_rgb565).
 * ------------------------------------------------------------------------- */
#define DET_HERO_Y   42                   /* art hero top                         */
#define DET_ART      56                   /* hero art dimension                   */
#define DET_LIST_Y0  108                  /* tracklist first row top              */
#define DET_ROWS     5                     /* visible rows: (240-108)/24 = 5 fit   */

/* The raw folder.art staging buffer is the ART CACHE's — see artcache_scratch().
 * There used to be a second, byte-identical ART_RAW_MAX array here: 28,812 bytes
 * duplicated for no reason, since the cache's copy is only live inside
 * artcache_pump() and this path (entering an album) never pumps. */
#define ART_RAW_MAX  ARTCACHE_SCRATCH_SZ
static uint16_t g_detail_art[DET_ART * DET_ART]; /* downscaled 56x56 hero           */
static int      g_detail_art_ok;
static char     g_album_title[NAME_MAX + 1];     /* album part of "Artist - Album" */
static char     g_album_artist[NAME_MAX + 1];    /* artist part (empty if none)    */
static int      g_album_track_n;                  /* playable files in the folder   */

/* Per-track disc/duration (from the index) + a display "view" that interleaves
 * "Disc N" header rows for multi-disc albums. Filled in detail_load_meta. A view
 * entry >= 0 is a track index into g_browse; < 0 encodes a header disc as
 * -(disc+1). */
#define DET_VIEW_MAX (BROWSE_MAX + 8)
static uint16_t g_track_dur[BROWSE_MAX];
static uint16_t g_track_num[BROWSE_MAX];
static uint8_t  g_track_disc[BROWSE_MAX];
/* Real (tag) title per browse row, bound from the index in detail_load_meta.
 * The on-disk filename is FAT-sanitized (?,*,:,/ -> _), so the tracklist shows
 * this instead when available; NULL falls back to the filename. */
static const char *g_track_title[BROWSE_MAX];
static int16_t  g_det_view[DET_VIEW_MAX];
static int      g_det_view_n;
static int      g_detail_multidisc;
/* Total runtime of the folder's tracks, summed ONCE in detail_load_meta (the
 * meta line used to re-sum every track on every paint). 0 = durations unknown. */
static uint32_t g_detail_total_s;

/* Read the current folder's folder.art (clus/size captured by browse_load) and
 * downscale it to the 56x56 hero. Leaves g_detail_art_ok=0 if absent/malformed.
 * Same CoreArt "CART" validation the player uses (player.c load_folder_art). */
static void detail_art_load(fat32_t *fs)
{
    g_detail_art_ok = 0;
    if (g_art_clus == 0 || g_art_size < 12 || g_art_size > (uint32_t)ART_RAW_MAX) {
        return;
    }
    uint8_t *raw = artcache_scratch();
    int32_t  n   = fat32_read_file(fs, g_art_clus, raw, g_art_size);
    if (n < 12) {
        return;
    }
    if (raw[0] != 'C' || raw[1] != 'A' || raw[2] != 'R' || raw[3] != 'T') {
        return;
    }
    int w = raw[6] | (raw[7] << 8);
    int h = raw[8] | (raw[9] << 8);
    if (w <= 0 || h <= 0 || w > 120 || h > 120) {
        return;
    }
    if ((int32_t)(12 + w * h * 2) > n) {
        return;
    }
    thumb_box_rgb565((const uint16_t *)(raw + 12), w, h,
                     g_detail_art, DET_ART, DET_ART);
    g_detail_art_ok = 1;
}

/* Animated three-bar "now playing" glyph (collection-detail.jsx NowPlayingDot).
 * Each bar bounces on a triangle wave with its own phase; bottom-anchored at
 * y+9, heights 3..9. `t_us` is the free-running microsecond clock. */
#define NP_BARS_W  9                          /* bounding box width  (x..x+8)      */
#define NP_BARS_H  9                          /* bounding box height (y..y+8)      */

static int nowplaying_bar_h(uint32_t t_ms, int phase)
{
    uint32_t p = (t_ms + (uint32_t)phase) % 760u;      /* period 760 ms          */
    int tri = (p < 380u) ? (int)p : (int)(760u - p);   /* 0..380..0 triangle     */
    return 3 + (tri * 6) / 380;                          /* 3..9 px               */
}

/* The bars' own clock: the USEC_TIMER less every pause so far. While the
 * player is paused it stands still, so the bars freeze mid-bounce and carry
 * on from the same shape when the music does — the owner's rule, "the bars
 * should only be moving if music is actually playing", which the desktop app
 * follows too (its NowPlaying.BarsPausedAt). Before this they were keyed on
 * player_active(), which stays 1 across a pause, so a paused track's bars
 * kept bouncing. Self-contained: every caller reads the time through here. */
static uint32_t bars_clock(void)
{
    static uint32_t hold_us, pause_at;
    static int      was_paused;
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    int paused = player_paused();
    if (paused) {
        if (!was_paused) pause_at = now;
        was_paused = 1;
        return pause_at - hold_us;
    }
    if (was_paused) {
        hold_us += now - pause_at;
        was_paused = 0;
    }
    return now - hold_us;
}

static void nowplaying_bars(int x, int y, uint16_t c, uint32_t t_us)
{
    uint32_t ms = t_us / 1000u;
    static const int ph[3] = { 0, 250, 500 };
    for (int i = 0; i < 3; i++) {
        int h = nowplaying_bar_h(ms, ph[i]);
        console_fill_rect(x + i * 3, y + (NP_BARS_H - h), 2, h, c);
    }
}

/* One tracklist row of the album detail, at list-row `r` showing view entry
 * `vi`. Split out of detail_render so a selection move can repaint just the two
 * rows that changed instead of the whole panel (see list_repaint_partial). */
static void detail_row_draw(int r, int vi)
{
    int ry = DET_LIST_Y0 + r * ROW_H;
    int16_t v = g_det_view[vi];
    /* Single-disc albums have no "DISC N" header rows, so the per-disc number
     * gutter is wasted width — pull the numbers and titles left to reclaim it. */
    int num_rx  = g_detail_multidisc ? 30 : 24;   /* track-number right-align x */
    int title_x = g_detail_multidisc ? 38 : 30;   /* title left x               */
    if (v < 0) {                          /* a "Disc N" section header */
        char h[12];
        int hi = 0;
        for (const char *p = "DISC "; *p; p++) h[hi++] = *p;
        u32_to_dec(h + hi, (unsigned)(-(int)v - 1));
        ui_text(14, UI_ROW_BASE(ry, ROW_H), h, FONT_SMALL, LINEN_MUTED_D);
        return;
    }
    int idx = v;
    const browse_entry_t *e = &g_browse[idx];
    int is_sel = (idx == g_det_sel);
    if (is_sel) {
        ui_round_rect(6, ry + 1, LCD_WIDTH - 16, ROW_H - 2, 4, LINEN_SEL_BG);
    }
    uint16_t fg = is_sel ? LINEN_SEL_FG : LINEN_INK;
    uint16_t nc = is_sel ? LINEN_SEL_SUB : LINEN_MUTED2;
    /* Left gutter: the now-playing bars for the playing track, else its
     * (per-disc) track number (collection-detail.jsx). */
    const char *playing = player_active() ? player_track_name() : 0;
    if (playing && name_eq_ci(e->name, playing)) {
        nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK,
                        bars_clock());
    } else {
        char num[6];
        u32_to_dec(num, (unsigned)(g_track_num[idx] ? g_track_num[idx]
                                                    : idx + 1));
        int nw = text_width(num, FONT_SMALL);
        /* Regular-9 digits are 7 px, the title's caps 10: on the title's
         * baseline the number hung 1.5 px low. Centre its own cap band. */
        ui_text(num_rx - nw, UI_ROW_BASE_FOR(ry, ROW_H, FONT_SMALL), num,
                FONT_SMALL, nc);
    }
    /* Duration on the right (from the index); the title must stop before it. */
    int title_right = LCD_WIDTH - 16;
    if (g_track_dur[idx]) {
        char dts[FMT_TIME_MAX];
        fmt_time(dts, g_track_dur[idx]);
        int dw = text_width(dts, text_font_bold_12());
        /* Bold-12 figures (8 px) centred on the row in their own right, the
         * way ui_list_row places every right-hand value. */
        ui_text(LCD_WIDTH - 16 - dw, UI_ROW_BASE_FOR(ry, ROW_H, text_font_bold_12()),
                dts, text_font_bold_12(), is_sel ? LINEN_SEL_SUB : LINEN_MUTED_D);
        title_right = LCD_WIDTH - 16 - dw - 8;
    }
    /* Title (clean — number gutter provides the index), indented past it,
     * CLIPPED before the duration so a long title can't overlap it; the
     * selected row's long title scrolls (marquee). */
    const text_font_t *tf = is_sel ? FONT_HEADER : FONT_ROW;
    /* Prefer the real tag title (bound from the index): the filename is
     * FAT-sanitized (e.g. "WHO CARES?" -> "WHO CARES_"), the tag isn't. */
    const char *tt = g_track_title[idx] ? g_track_title[idx]
                                        : track_display(e->name);
    if (is_sel) {
        mq_text(title_x, UI_ROW_BASE(ry, ROW_H), title_right - title_x, tt, tf, fg,
                LINEN_SEL_BG, ry, ry + ROW_H);
    } else {
        ui_text_clip(title_x, UI_ROW_BASE(ry, ROW_H), tt, tf, fg, title_x, title_right);
    }
}

/* The selected track's position in the display view (which interleaves the
 * "Disc N" header rows), i.e. the row space the scroll window works in. */
static int detail_sel_view(int sel)
{
    for (int i = 0; i < g_det_view_n; i++) {
        if (g_det_view[i] == (int16_t)sel) return i;
    }
    return 0;
}

static void detail_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    fmt_count(right, sel + 1, g_browse_n > 0 ? g_browse_n : 1);
    ui_header("Albums", right, 1);

    /* Hero art (or a placeholder tile when the folder has no folder.art). */
    if (g_detail_art_ok) {
        console_blit565(12, DET_HERO_Y, DET_ART, DET_ART, g_detail_art);
    } else {
        console_fill_rect(12, DET_HERO_Y, DET_ART, DET_ART, LINEN_BORDER);
    }
    int tx = 12 + DET_ART + 12;
    ui_text(tx, DET_HERO_Y + 15, g_album_title, FONT_HEADER, LINEN_INK);
    if (g_album_artist[0]) {
        ui_text(tx, DET_HERO_Y + 31, g_album_artist, FONT_SUB, LINEN_MUTED_D);
    }
    char meta[40];
    int mi = u32_to_dec(meta, (unsigned)g_album_track_n);
    for (const char *p = (g_album_track_n == 1) ? " track" : " tracks"; *p; p++)
        meta[mi++] = *p;
    /* Append total runtime ("11 tracks · 1h 41m") when durations are known
     * (index path). Summed ONCE in detail_load_meta — this runs on every paint.
     * collection-detail.jsx meta line. */
    if (g_detail_total_s > 0) {
        for (const char *p = " " UI_GLYPH_MIDDOT " "; *p; p++) meta[mi++] = *p;
        uint32_t tot_m = (g_detail_total_s + 59u) / 60u;  /* round UP to minutes */
        uint32_t hh = tot_m / 60u, mm = tot_m % 60u;
        if (hh > 0) {
            mi += u32_to_dec(meta + mi, hh);
            meta[mi++] = 'h'; meta[mi++] = ' ';
        }
        mi += u32_to_dec(meta + mi, mm);
        meta[mi++] = 'm';
    }
    meta[mi] = '\0';
    ui_text(tx, DET_HERO_Y + 47, meta, FONT_SMALL, LINEN_MUTED2);

    console_fill_rect(12, DET_LIST_Y0 - 6, LCD_WIDTH - 24, 1, LINEN_BORDER);

    if (g_browse_n == 0) {
        ui_text(14, DET_LIST_Y0 + 14, "Empty folder", FONT_ROW, LINEN_MUTED);
        return;
    }
    /* Scroll over the display view (tracks + any "Disc N" headers), centered on
     * the selected track's position within it. */
    int top = ui_scroll_window(detail_sel_view(sel), g_det_view_n, DET_ROWS);
    for (int r = 0; r < DET_ROWS; r++) {
        int vi = top + r;
        if (vi >= g_det_view_n) break;
        detail_row_draw(r, vi);
    }
    ui_scrollbar(DET_LIST_Y0, top, DET_ROWS, g_det_view_n);
}

/* A neutral tile shown in a row's chip slot until its real cover loads, so the
 * text doesn't shift right when the thumbnail pops in. Filled once at startup. */
static uint16_t g_chip_ph[ARTCACHE_DIM * ARTCACHE_DIM];
/* Diagnostic placeholders — see albumlist_row_draw. */
static uint16_t g_chip_noclus[ARTCACHE_DIM * ARTCACHE_DIM];
static uint16_t g_chip_noart[ARTCACHE_DIM * ARTCACHE_DIM];

static void chip_placeholder_init(void)
{
    for (int i = 0; i < ARTCACHE_DIM * ARTCACHE_DIM; i++) {
        g_chip_ph[i]     = LINEN_BORDER;
        g_chip_noclus[i] = 0xFD20;             /* amber: no cluster resolved   */
        g_chip_noart[i]  = 0xF800;             /* red:   load gave up          */
    }
}

/* Queue every album row's cover into the incremental thumbnail cache. Called
 * when the album list is (re)entered; artcache_pump loads them a few frames
 * apart off the audio path. NOT reset here — artcache_queue is idempotent (a
 * slot whose cover is unchanged keeps its loaded pixels), so backing out of an
 * album detail re-uses the covers already loaded instead of reloading them all.
 * A changed view (e.g. a different artist filter) re-queues only the slots whose
 * album actually changed. */
static void albumlist_queue_chips(void)
{
    for (int i = 0; i < g_albumview_n; i++) {
        int a = g_albumview[i];
        if (a >= ARTCACHE_SLOTS) continue;
        const lib_album_t *al = &g_albums[a];
        artcache_queue(a, al->thm_clus, al->thm_size, al->art_clus, al->art_size);
    }
}

/*
 * The album list carries a synthetic "All Songs" row at index 0 WHEN it is
 * showing a single artist — browsing an artist by album alone makes a track you
 * remember but cannot place hard to reach. Row 0 is the artist's whole
 * discography in title order; rows 1.. are the albums.
 *
 * Everything that indexes the list has to agree about that offset, so it is
 * expressed once here and every caller goes through albumlist_album_at().
 */
static int albumlist_all_row(void)
{
    return g_artist_filter[0] ? 1 : 0;
}

static int albumlist_count(void)
{
    return g_albumview_n + albumlist_all_row();
}

/* Global g_albums[] index for a list row, or -1 for the "All Songs" row. */
static int albumlist_album_at(int row)
{
    int k = row - albumlist_all_row();
    if (k < 0 || k >= g_albumview_n) {
        return -1;
    }
    return g_albumview[k];
}

/* One album row at list-row `r` showing album-view entry `idx`. Split out of
 * albumlist_render so a selection move can repaint just the rows that changed. */
static void albumlist_row_draw(int r, int idx)
{
    int a = albumlist_album_at(idx);
    if (a < 0) {                            /* the synthetic "All Songs" row */
        list_row_tall(r, "All Songs", 0, 0, 1, idx == g_br_sel, 0, 0);
        return;
    }
    const lib_album_t *e = &g_albums[a];
    /* (Re)queue the visible rows every paint — idempotent for a slot that
     * already holds this album, and it's what gets a cover for rows past the
     * bulk prefetch in albumlist_queue_chips when the library has more albums
     * than the cache has slots.
     *
     * Keyed by the GLOBAL album index, not the row: the row number shifts when
     * an artist filter adds the "All Songs" row, and it means a different album
     * under a different filter — so a row-keyed cache aliases across filters. */
    artcache_queue(a, e->thm_clus, e->thm_size, e->art_clus, e->art_size);
    const uint16_t *chip = artcache_get(a);
    if (!chip) {
        /* DIAGNOSTIC: tint the placeholder by WHY there are no pixels, so a
         * blank chip is self-describing on a device with no serial cable.
         *   grey  = still coming (queued)
         *   amber = no sidecar cluster was ever resolved for this album
         *   red   = tried to load it and gave up
         * Remove once the first-album blank is understood. */
        switch (artcache_state(a)) {
        case ARTCACHE_ST_NOCLUS: chip = g_chip_noclus; break;
        case ARTCACHE_ST_NOART:  chip = g_chip_noart;  break;
        default:                 chip = g_chip_ph;     break;
        }
    }
    /* Show "Album" as the title and the artist as a sub-line (parsed from the
     * "Artist - Album" folder name). In an artist's own list the artist sub
     * is redundant, so drop it there. */
    char artist[NAME_MAX + 1], album[NAME_MAX + 1];
    split_artist_album(e->folder, artist, album);
    const char *sub = (!g_artist_filter[0] && artist[0]) ? artist : 0;
    list_row_tall(r, album, sub, 0, 1, idx == g_br_sel, 0, chip);
}

/* Album LIST (browser depth 0): the folder list with the design chrome, each
 * album row carrying a 22x22 cover chip (or a placeholder until it loads). */
static void albumlist_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    int total = albumlist_count();
    if (total > 0) {
        fmt_count(right, sel + 1, total);
    } else {
        right[0] = '\0';
    }
    /* Header title = the artist when drilled in from Artists, else "Albums". */
    ui_header(g_artist_filter[0] ? g_artist_filter : "Albums", right, 1);

    if (total == 0) {
        ui_text(14, LIST_Y0 + 20, "No albums", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, total, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= total) break;
        albumlist_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, total);
}

static int  g_artist_sel, g_artist_accum;

static void artists_row_draw(int r, int idx)
{
    list_row(r, g_artists[idx].name, 0, 0, 1, idx == g_artist_sel, 0, 0);
}

static void artists_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_artists_n > 0) fmt_count(right, sel + 1, g_artists_n);
    else                 right[0] = '\0';
    ui_header("Artists", right, 1);

    if (g_artists_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No artists", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_artists_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_artists_n) break;
        artists_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, g_artists_n);
}

static void browse_render(int sel)
{
    if (g_dir_depth > 0) {
        detail_render(sel);
    } else {
        albumlist_render(sel);
    }
}

int        g_song_sel, g_song_accum;
static int        g_genre_sel, g_genre_accum;

/*
 * The album a song belongs to, resolved through its folder cluster and split
 * out of the "Artist - Album" folder name. Writes "" when the folder isn't in
 * the album table (a track whose folder never became an album entry).
 *
 * Through album_by_clus: the cluster->album hash already exists (the loader
 * builds it for the queue builders), so there is no second table to keep in
 * sync. This used to be its own linear scan over every album, run per visible
 * row per repaint — up to 1024 compares a row, six rows a frame, on the one
 * list the wheel moves fastest.
 */
static void song_album_title(const lib_song_t *sg, char *out)
{
    int ai = album_by_clus(sg->dir_clus);
    if (ai >= 0) {
        char artist[NAME_MAX + 1];
        split_artist_album(g_albums[ai].folder, artist, out);
        return;
    }
    out[0] = '\0';
}

static void songs_row_draw(int r, int idx)
{
    lib_song_t *sg = &g_songs[g_songview[idx]];
    char dur[FMT_TIME_MAX];
    if (sg->duration_s) fmt_time(dur, sg->duration_s); else dur[0] = '\0';

    /*
     * Sub-line: normally the artist, because the unfiltered Songs list mixes
     * every artist together and that is what tells rows apart.
     *
     * In an artist's "All Songs" the header ALREADY names the artist, so
     * repeating it on every row spends the only sub-line on the one fact the
     * user cannot need. Show the ALBUM there instead — in that view it is
     * exactly what distinguishes one row from the next.
     */
    char alb[NAME_MAX + 1];
    const char *sub = sg->artist[0] ? sg->artist : 0;
    if (g_songview_artist[0]) {
        song_album_title(sg, alb);
        sub = alb[0] ? alb : 0;
    }
    list_row_titled(r, sg->title, sub, dur[0] ? dur : 0, idx == g_song_sel, 0);
}

static void songs_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_songview_n > 0) fmt_count(right, sel + 1, g_songview_n);
    else                  right[0] = '\0';
    ui_header(g_songview_artist[0] ? g_songview_artist : "Songs",
                  right, 1);
    if (g_songview_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No songs", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_songview_n, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= g_songview_n) break;
        songs_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, g_songview_n);
}

static void genres_row_draw(int r, int idx)
{
    char cnt[8];
    u32_to_dec(cnt, (unsigned)g_genre_count[idx]);
    list_row(r, g_genres[idx], 0, cnt, 0, idx == g_genre_sel, 0, 0);
}

static void genres_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_genres_n > 0) fmt_count(right, sel + 1, g_genres_n);
    else                right[0] = '\0';
    ui_header("Genres", right, 1);
    if (g_genres_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No genres", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_genres_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_genres_n) break;
        genres_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, g_genres_n);
}

/* ---------------------------------------------------------------------------
 * Queue view: the tracks queued in the current playback (the folder a track was
 * launched from). The playing track carries the animated now-playing bars;
 * SELECT jumps to a track. Reached via SELECT on the Now Playing screen.
 * ------------------------------------------------------------------------- */
static int g_queue_sel, g_queue_accum;

static void queue_row_draw(int r, int idx)
{
    int is_sel = (idx == g_queue_sel);
    list_row(r, track_display(player_queue_name(idx)), 0, 0, 0, is_sel,
             player_queue_is_dir(idx), 0);
    if (idx == player_queue_current()) {      /* the currently-playing track */
        int ry = LIST_Y0 + r * ROW_H;
        nowplaying_bars(LCD_WIDTH - 22, ry + 7,
                        is_sel ? LINEN_SEL_FG : LINEN_INK,
                        bars_clock());
    }
}

static void queue_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    int n   = player_queue_len();
    int cur = player_queue_current();
    char right[12];
    if (n > 0) fmt_count(right, cur + 1, n);
    else       right[0] = '\0';
    ui_header("Now Playing", right, 1);

    if (n == 0) {
        ui_text(14, LIST_Y0 + 20, "Queue empty", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= n) break;
        queue_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, n);
}

/* ---------------------------------------------------------------------------
 * Settings (core/ui/settings.h data-driven model). g_settings holds the state;
 * g_set_screen tracks the current sub-screen within the one SCR_SETTINGS stack
 * entry. Slider rows use a brief edit mode (SELECT toggles it, then the wheel
 * adjusts) so the wheel can still move the selection otherwise.
 * ------------------------------------------------------------------------- */
static int g_set_screen;                  /* current settings_screen_t          */
static int g_set_sel;                     /* selection within g_set_screen      */
static int g_set_root_sel;                /* saved ROOT selection               */
static int g_set_accum;                   /* wheel accumulator                  */
static int g_set_editing;                 /* editing a slider row               */

/* The UI's band count and the HAL's are the same silicon's; a curve is copied
 * across the boundary as a plain array, so a disagreement would be a buffer
 * overrun rather than a compile error. */
_Static_assert(EQ_BANDS == EQ_BAND_COUNT, "ui/eq.h and wm8758.h disagree");

/* Push the FUNCTIONAL settings out to the subsystems. Cosmetic fields are a
 * no-op. The backlight timeout/brightness are read live by the loop. */
static void settings_apply(void)
{
    player_set_shuffle(g_settings.shuffle);
    player_set_repeat((int)g_settings.repeat);
    /* The volume ceiling goes through the same helper the Now Playing wheel
     * uses. config_decode and settings_adjust both keep volume <= limit, so
     * this only ever bites on a record from somewhere else -- and then it
     * writes the clamped value back, rather than leaving the Settings slider
     * showing a level the codec is not being given. */
    g_volume = settings_volume_clamp(&g_settings, g_settings.volume);
    g_settings.volume = g_volume;
    hal_volume_set(g_volume);
    hal_balance_set(g_settings.balance);
    /* A preset owns both shelves; at EQ Off it is the Bass/Treble sliders. */
    eq_curve_t curve;
    eq_effective_curve(g_settings.eq, g_settings.bass, g_settings.treble,
                       &curve);
    hal_eq_set(curve.gain_db, curve.cutoff, curve.narrow);
    theme_set(g_settings.theme);           /* Linen / Onyx -> live palette swap */
    chip_placeholder_init();               /* re-bake LINEN_BORDER: the album
                                            * list's empty chips follow a real
                                            * theme change (a preview does not
                                            * reach here — theme_preview_sync) */
    charger_apply();                       /* Charge Rate -> LTC4066 HPWR       */
}

/*
 * Bring the running countdown in step with the Settings row (the invariant at
 * g_sleep). Called after anything that MOVES g_settings.sleep_timer_min: the
 * row's SELECT and Reset Settings. Deliberately NOT called from the boot path
 * — .bss and config_decode both leave the field at 0, so there is nothing to
 * arm, and calling it there would be the one place a stale record could.
 *
 * The guard is on the VALUE, so an apply that finds the two sides already in
 * step does nothing and cannot restart a countdown by accident. The row's
 * SELECT always moves the value (it cycles), so every press does re-arm —
 * including the sixth, which lands back where it started and is plainly meant
 * as a fresh start.
 */
static void sleep_timer_apply(void)
{
    if (g_settings.sleep_timer_min == sleeptimer_total_min(&g_sleep)) {
        return;
    }
    sleeptimer_arm(&g_sleep, g_settings.sleep_timer_min,
                   mmio_read32(USEC_TIMER_ADDR));
    if (g_settings.sleep_timer_min > 0) {
        uart_puts("core: sleep timer: armed ");
        uart_dec(g_settings.sleep_timer_min);
        uart_puts(" min\n");
    } else {
        uart_puts("core: sleep timer: off\n");
    }
}

/* ---------------------------------------------------------------------------
 * On-The-Go, the parts the Playlists screens need. The rest of the feature is
 * a section of its own below (it needs the screen stack); these few things sit
 * here because Playlists pins an On-The-Go row and a saved slot's tracklist
 * carries a Delete row, and both are painted from here.
 * ------------------------------------------------------------------------- */
otg_list_t g_otg;                  /* the live list, 4100 B of .bss   */
static int16_t    g_otg_song[OTG_MAX];    /* the g_songs index each entry
                                           * binds to, -1 = not on this iPod */
static int        g_otg_missing;          /* how many of those there are     */

/*
 * A destructive row asks twice. The first press re-labels the row and starts a
 * three-second window; the second press inside it acts. Two presses because
 * the live list is the only copy of something assembled by hand and a saved
 * slot is the only copy of something saved, and there is no undo — and a
 * confirm the row itself carries needs no modal, no second screen and no new
 * convention.
 */
#define OTG_CONFIRM_US 3000000u

static ui_window_t g_otg_confirm;       /* Clear Playlist, on SCR_OTG        */
ui_window_t g_pl_confirm;        /* Delete Playlist, on a saved slot  */

int otg_confirm_up(ui_window_t *w)
{
    return ui_window_up(w, OTG_CONFIRM_US, mmio_read32(USEC_TIMER_ADDR));
}

/* Bounded string copy; nothing in lib/ has one and three little formatters
 * below want it. */
static void otg_str_copy(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    for (; src && src[i] && i + 1u < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * On-The-Go: the playlist you build while walking around.
 *
 * Hold Select on a song (or an album) and it joins a LIVE list; Playlists >
 * On-The-Go opens it, plays it, clears it or SAVES it into one of five slot
 * playlists on the disk. The two on-disk halves are host-tested modules —
 * kernel/otg_store.c for the live list (COREOTG.DAT) and library/otg_slot.c
 * for the saved ones — and core/docs/design/on-the-go.md is the format
 * reference. What lives HERE is only what needs g_songs and g_folder_map:
 * turning a song into a locator pair, turning a locator pair back into a
 * song, and the screen.
 *
 * An entry is a pair of folded name hashes, which is what makes the whole
 * 512-entry list one 5 KB disk slot — see library/otg.h. The price is that an
 * entry has to be BOUND to a song at every boot and after every library
 * reload, and that an entry whose file is no longer on the disk resolves to
 * nothing. Such an entry is KEPT, drawn greyed with "Not on this iPod",
 * skipped when the queue is built and counted for the header: a later sync
 * may bring the file back, and silently dropping a row the user added is the
 * kind of data loss this project refuses everywhere else.
 * ------------------------------------------------------------------------- */

static int                g_otg_sel, g_otg_accum; /* the On-The-Go screen      */
static otg_save_row_t     g_otg_rows[OTG_MAX];    /* Save's row array, 4 KB    */
static otg_entry_t        g_otg_batch[OTG_MAX];   /* one album, in order, 4 KB */
static int16_t            g_otg_order[OTG_MAX];   /* ...sorted first, 1 KB     */
static otg_save_scratch_t g_otg_save_scr;         /* ~9.7 KB of .bss, one copy */
static otg_save_stats_t   g_otg_save_stats;
static int                g_otg_slot_lba_said;    /* the first-Save UART line  */

/* The pending change to COREOTG.DAT. The gate itself is inside
 * kernel/otg_store.c (as the event log's is inside evlog.c), so this file only
 * gathers what the gate needs and narrates the verdict. */
static void otg_touch(void)
{
    otg_store_touch(mmio_read32(USEC_TIMER_ADDR));
}

/* SCR_OTG's two action rows sit above the tracks, so a row index is
 * `2 + entry`. Named rather than spelled 2 everywhere: every renderer, the
 * list-view table and both Select bodies count in this space. */
enum { OTG_ROW_CLEAR = 0, OTG_ROW_SAVE = 1, OTG_ROW_FIRST = 2 };

static int otg_row_count(void) { return OTG_ROW_FIRST + (int)g_otg.n; }

/* ---- the confirmation banner ------------------------------------------- */

/*
 * A one-second banner over the top chrome — the Hold banner's own primitive
 * (top_banner_render) with a + or - in a circle instead of the padlock. It
 * exists because a hold-to-add has no other feedback: the list is on another
 * screen, and without this the user cannot tell a press that registered from
 * one that did not.
 *
 * Unlike the LOCKED banner it is not modal: the first real input dismisses it
 * (the UNLOCKED banner's rule), so nothing is ever applied unseen behind it.
 */
#define OTG_FLASH_US 900000u

static ui_window_t g_otg_flash;
int8_t      g_otg_flash_sign;            /* +1 plus, -1 minus, 0 none  */
char        g_otg_flash_label[40];
char        g_otg_flash_token[16];

/* "<n> SONGS" / "1 SONG" — the small-caps token where a list header's count
 * would be. */
static void otg_count_token(char *dst, int n)
{
    int i = u32_to_dec(dst, (unsigned)n);
    otg_str_copy(dst + i, 16u - (uint32_t)i, n == 1 ? " SONG" : " SONGS");
}

static void otg_flash(int sign, const char *label, const char *token)
{
    g_otg_flash_sign = (int8_t)sign;
    otg_str_copy(g_otg_flash_label, sizeof g_otg_flash_label, label);
    otg_str_copy(g_otg_flash_token, sizeof g_otg_flash_token, token ? token : "");
    ui_window_arm(&g_otg_flash);
}

/* ---- binding an entry to a song ---------------------------------------- */

/*
 * The locator pair for a library song: the folded hash of its album folder's
 * FULL on-disk name, and of its own. Returns 0 when the song cannot be named
 * that way — a lossy long name (no locator at all), or a library built by the
 * TAG-SCAN fallback, which leaves g_folder_map empty and so has no folder
 * hashes. The documented layout always has an index; the caller says "Not in
 * the library" rather than storing a pair that binds to nothing.
 */
static int otg_entry_of_song(int si, otg_entry_t *e)
{
    if (si < 0 || si >= g_songs_n) return 0;
    const lib_song_t *s = &g_songs[si];
    if (s->file_hash == 0) return 0;
    for (int i = 0; i < g_folder_n; i++) {
        if (g_folder_map[i].clus != s->dir_clus) continue;
        e->folder_hash = g_folder_map[i].hash;
        e->file_hash   = s->file_hash;
        return 1;                      /* file_hash != 0, so never the null pair */
    }
    return 0;
}

/* The song an entry names, or -1. The index's own binding rule: the folder
 * hash resolves to a cluster, then the file hash within it. */
static int otg_bind_one(int i)
{
    uint32_t fh = g_otg.e[i].file_hash;
    uint32_t dc = folder_clus_h(g_otg.e[i].folder_hash, "");
    if (dc == 0 || fh == 0) return -1;
    for (int k = g_song_hh[fh & (SONG_HASH_BUCKETS - 1)]; k; k = g_song_hn[k - 1]) {
        const lib_song_t *s = &g_songs[k - 1];
        if (s->file_hash == fh && s->dir_clus == dc) return k - 1;
    }
    return -1;
}

/* Re-bind the whole list. Called after every library load and after every
 * mutation, because both can change what an entry resolves to. */
void otg_bind_all(void)
{
    g_otg_missing = 0;
    for (int i = 0; i < (int)g_otg.n; i++) {
        g_otg_song[i] = (int16_t)otg_bind_one(i);
        if (g_otg_song[i] < 0) g_otg_missing++;
    }
}

/* ---- adding ------------------------------------------------------------ */

/* Add one library song, with the banner that says so. Returns 1 when the list
 * grew. */
static int otg_add_song(int si)
{
    otg_entry_t e;
    if (!otg_entry_of_song(si, &e)) {
        otg_flash(0, "Not in the library", 0);
        return 0;
    }
    if (!otg_add(&g_otg, e.folder_hash, e.file_hash)) {
        otg_flash(0, "On-The-Go is full", "512");
        return 0;
    }
    g_otg_song[g_otg.n - 1] = (int16_t)otg_bind_one((int)g_otg.n - 1);
    if (g_otg_song[g_otg.n - 1] < 0) g_otg_missing++;
    char tok[16];
    otg_count_token(tok, (int)g_otg.n);
    otg_flash(+1, "Added to On-The-Go", tok);
    otg_touch();
    return 1;
}

/*
 * Add a whole album, in the order its tracklist shows it: (disc, track), the
 * key browse_bind sorts by, with the index's own order as the tie-break. Runs
 * over g_songs rather than a directory listing, so it costs no disk read —
 * which is the point of holding Select on an album ROW.
 */
static uint32_t otg_track_key(int si)
{
    return ((uint32_t)g_songs[si].disc << 16) | g_songs[si].track;
}

static int otg_add_album(int ai)
{
    if (ai < 0 || ai >= g_albums_n) return 0;
    uint32_t dc = g_albums[ai].clus;
    int n = 0;
    for (int i = 0; i < g_songs_n && n < (int)OTG_MAX; i++) {
        if (g_songs[i].dir_clus != dc) continue;
        /* Insertion sort by (disc, track), the key browse_bind sorts a
         * tracklist by — so the album goes in the order its tracklist shows
         * it, filenames notwithstanding. Indices, not entries: the key is
         * read out of g_songs, so the sort needs no second array. An album is
         * tens of tracks and this runs once per press. */
        uint32_t key = otg_track_key(i);
        int j = n;
        while (j > 0 && otg_track_key(g_otg_order[j - 1]) > key) {
            g_otg_order[j] = g_otg_order[j - 1];
            j--;
        }
        g_otg_order[j] = (int16_t)i;
        n++;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (otg_entry_of_song(g_otg_order[i], &g_otg_batch[m])) m++;
    }
    if (m == 0) {
        otg_flash(0, "Not in the library", 0);
        return 0;
    }
    int before = (int)g_otg.n;
    int added  = otg_add_many(&g_otg, g_otg_batch, m);
    if (added == 0) {
        otg_flash(0, "On-The-Go is full", "512");
        return 0;
    }
    for (int i = before; i < (int)g_otg.n; i++) {
        g_otg_song[i] = (int16_t)otg_bind_one(i);
        if (g_otg_song[i] < 0) g_otg_missing++;
    }
    char label[40];
    int  k = 0;
    otg_str_copy(label, sizeof label, "Added ");
    k = 6;
    k += u32_to_dec(label + k, (unsigned)added);
    otg_str_copy(label + k, sizeof label - (uint32_t)k,
                 added == 1 ? " song" : " songs");
    otg_flash(+1, label, "ON-THE-GO");
    otg_touch();
    return 1;
}

/* ---- playing ----------------------------------------------------------- */

/*
 * Play the live list as the queue, from entry `start`. Unresolved entries are
 * skipped (they name no file), so the queue index the pick lands on is not the
 * entry index — which is why this returns it. RESUME_KIND_OTG carries no
 * context hash: the list is not a file and has no name, and the boot path
 * rebuilds it from COREOTG.DAT.
 */
static int otg_play(int start)
{
    if (g_otg.n == 0) return -1;
    g_queue_kind     = RESUME_KIND_OTG;
    g_queue_seed     = 0;
    g_queue_ctx_hash = 0;
    player_set_shuffle(g_settings.shuffle);
    player_queue_begin();
    int added = 0, qi = 0;
    for (int i = 0; i < (int)g_otg.n; i++) {
        /* The start position is recorded BEFORE the resolved check, so a pick
         * whose file is no longer on the disk lands on the NEXT entry that is
         * rather than on the top of the list — library_play_song's rule. */
        if (i == start) qi = added;
        int si = g_otg_song[i];
        if (si < 0 || g_songs[si].file_clus == 0) continue;
        browse_entry_t e;
        queue_entry_from_song(&e, &g_songs[si]);
        player_queue_add(&e);
        added++;
    }
    if (added == 0) return -1;
    if (qi >= added) qi = added - 1;       /* nothing resolved after it */
    player_queue_commit(qi);
    return qi;
}

/* ---- the slot files ---------------------------------------------------- */

/* Find the Playlists folder under the library root. 0 when there is none —
 * which is what a volume the host never synced looks like. */
typedef struct { const char *want; uint32_t clus; } otg_find_t;

static int otg_find_dir_cb(void *ud, const fat32_dirent_t *e)
{
    otg_find_t *f = (otg_find_t *)ud;
    if (e->is_dir && name_eq_ci(e->name, f->want)) { f->clus = e->first_clus; return 1; }
    return 0;
}

static uint32_t otg_playlists_dir(fat32_t *fs)
{
    otg_find_t f;
    f.want = PLAYLIST_DIR;
    f.clus = 0;
    if (fat32_readdir(fs, playlists_root_clus(fs), otg_find_dir_cb, &f) != 0) return 0;
    return f.clus;
}

/*
 * Locate slot file `n` (1..OTG_SLOTS) by name. Deliberately NOT through
 * playlist_scan: the slot Save wants is the EMPTY one, and playlists_load
 * hides exactly those. Two directory reads, no 5 KB array.
 */
static int otg_slot_file(fat32_t *fs, int n, uint32_t *clus, uint32_t *size)
{
    *clus = *size = 0;
    uint32_t dir = otg_playlists_dir(fs);
    if (dir == 0) return 0;
    char name[OTG_SLOT_NAME_BYTES + 8];
    otg_slot_name(name, n);
    if (name[0] == '\0') return 0;
    otg_str_copy(name + 11, sizeof name - 11u, ".m3u8");

    /* The size has to come off the directory entry, and fat32_open_in gives
     * it along with the cluster. */
    uint32_t c = 0, sz = 0;
    if (fat32_open_in(fs, dir, name, &c, &sz) != 0) return 0;
    *clus = c;
    *size = sz;
    return 1;
}

/*
 * The lowest slot Save may use: one that EXISTS, carries the directive (a
 * foreign playlist at that name is the user's and is never written to), says
 * count 0, and is a size the writer will take. 0 when there is none.
 *
 * A DAMAGED slot is not offered here. Deciding that a used slot is torn needs
 * the parser's line count — a full read of all five on every Save and on every
 * paint of the greyed Save row — and the recovery is one press away: the
 * damaged slot opens to "Playlist damaged" with Delete Playlist under it.
 */
static int otg_slot_free_index(fat32_t *fs)
{
    for (int n = 1; n <= (int)OTG_SLOTS; n++) {
        uint32_t clus = 0, size = 0;
        if (!otg_slot_file(fs, n, &clus, &size)) continue;
        if (size < OTG_SLOT_FILE_MIN || (size % OTG_SLOT_SIZE_GRAIN) != 0) continue;
        otg_slot_info_t info;
        if (otg_slot_probe(fs, clus, size, &info) != 0) continue;
        if (info.present && info.count == 0) return n;
    }
    return 0;
}

/* ---- the On-The-Go screen ---------------------------------------------- */

/*
 * The slot Save would write into, 0 for none. Decided when the screen OPENS
 * (five 512-byte reads) rather than at paint time: the partial-repaint path
 * draws rows without going through otg_render, so a value recomputed there
 * would be stale exactly when a row is redrawn on its own.
 */
static int g_otg_free_slot;

static void otg_row_draw(int r, int idx)
{
    int sel = (idx == g_otg_sel);
    if (idx == OTG_ROW_CLEAR) {
        const char *label = otg_confirm_up(&g_otg_confirm) ? "Clear? Select again"
                                                           : "Clear Playlist";
        ui_list_row(LIST_Y0, r, label, 0, 0, 0, sel, g_otg.n == 0, 0, 1, ROW_H2);
        return;
    }
    if (idx == OTG_ROW_SAVE) {
        int greyed = (g_otg.n == 0) || (g_otg_free_slot == 0);
        const char *sub = (g_otg.n > 0 && g_otg_free_slot == 0)
                            ? "All five saved lists are in use" : 0;
        ui_list_row(LIST_Y0, r, "Save Playlist", sub, 0, 0, sel, greyed, 0, 1, ROW_H2);
        return;
    }
    int i  = idx - OTG_ROW_FIRST;
    int si = g_otg_song[i];
    if (si < 0) {
        /* Kept, not dropped: a later sync may bring the file back. The row
         * says why it cannot play instead of pretending it can. */
        ui_list_row(LIST_Y0, r, "Not on this iPod", 0, 0, 0, sel, 1, 0, 1, ROW_H2);
        return;
    }
    const lib_song_t *sg = &g_songs[si];
    const char *title = sg->title[0] ? sg->title : track_display(sg->file);
    const char *sub   = sg->artist[0] ? sg->artist : 0;
    char dur[FMT_TIME_MAX];
    dur[0] = '\0';
    if (sg->duration_s) fmt_time(dur, sg->duration_s);
    list_row_titled(r, title, sub, dur[0] ? dur : 0, sel, 0);
}

/* The header's right-hand value: "3 / 12" on a track row, "2 missing" on an
 * action row when some entry names a file that is not here, else nothing. */
static void otg_header_right(char *right, int sel)
{
    right[0] = '\0';
    if (sel >= OTG_ROW_FIRST && g_otg.n > 0) {
        fmt_count(right, sel - OTG_ROW_FIRST + 1, (int)g_otg.n);
    } else if (g_otg_missing > 0) {
        int k = u32_to_dec(right, (unsigned)g_otg_missing);
        otg_str_copy(right + k, 12u - (uint32_t)k, " MISSING");
    }
}

static void otg_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    otg_header_right(right, sel);
    ui_header("On-The-Go", right, 1);
    if (g_otg.n == 0) {
        /* The two action rows stay, greyed — an empty screen with nothing on
         * it reads as a bug, and the rows are what the screen IS — with the
         * sentence that says how a list is made underneath, because nothing
         * else on the device teaches the gesture. Drawn here rather than
         * through the row loop so the sentences sit under them; the list-view
         * table declines this state for the same reason, so it always
         * repaints whole. */
        otg_row_draw(0, OTG_ROW_CLEAR);
        otg_row_draw(1, OTG_ROW_SAVE);
        ui_text(14, LIST_Y0 + 2 * ROW_H2 + 22, "On-The-Go is empty",
                FONT_ROW, LINEN_MUTED);
        ui_text(14, LIST_Y0 + 2 * ROW_H2 + 42, "Hold Select on a song to add it",
                FONT_SMALL, LINEN_MUTED2);
        return;
    }
    int total = otg_row_count();
    int top   = ui_scroll_window(sel, total, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= total) break;
        otg_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, total);
}

/* Every Settings screen gets the same status strip as the lists: the painters
 * in ui/screen_settings.c leave rows 0..STATUS_H-1 clear (their console_clear
 * is what puts the surface there), so the strip is painted over that band here,
 * after the painter returns. */
static void settings_render_cur(void)
{
    /* Date & Time's first row shows the time it would edit, and ui/settings.c
     * is pure — it has no clock. Injected once per paint, the way the marquee's
     * clock is injected into ui/chrome.c. */
    {
        uint32_t local = 0;
        settings_set_now(clock_local_epoch(&local), local);
    }

    if (g_set_screen == SETTINGS_ABOUT) {
        /* The counts are capped (LIB_MAX_SONGS/ALBUMS, ARTISTS_MAX,
         * LIB_MAX_GENRES). When a load actually hit one of those caps the
         * screen says so — otherwise a library that's too big just looks
         * like it lost tracks. The warning and the load time are
         * INDEPENDENT (the warning used to hide the number entirely). */
        /* The jack token is the only probe this device has for the detect
         * line (ui/settings.h). Three reads, paid on this page alone: the
         * pin and its two configuration registers. The debounced half comes
         * from the loop's own last answer, so it reads -1 — and the token
         * drops that half — exactly while the line is untrusted. */
        about_jack_t jack = {
            .raw       = (int8_t)headphone_raw(),
            .debounced = g_jack.last,
            .pin_cfg   = (uint8_t)headphone_pin_cfg(),
            .edges     = g_jack.raw_edges,
        };
        settings_about_render(g_bat_pct, g_bat_mv, g_bat_raw, g_total_mb, g_free_mb,
                              g_songs_n, g_albums_n, g_artists_n,
                              evlog_seq(),
                              evlog_enabled()  ? ABOUT_LOG_ON :
                              evlog_failures() ? ABOUT_LOG_ERR : ABOUT_LOG_OFF,
                              g_lib_truncated, CORE_VERSION, &jack);
    } else if (g_set_screen == SETTINGS_DIAG) {
        /* Boot Details owns the diagnostics now: the cold-boot phase
         * breakdown and the settings-file locator. They used to be squeezed
         * into About's spare gaps, which is how the CFG rows ended up on top
         * of the stat columns. Read-only.
         *
         * The LBAs resolve FRESH here — exactly what a save would target,
         * rather than something cached — but ONLY while the drive is already
         * spinning, because every probe walks the FAT chain and this page is
         * repainted on every frame. With the drive parked, opening the page
         * paid a 1-3 s spin-up before its first pixel; it now shows the pair
         * probed at boot, which is the same address unless the file moved (it
         * does not — the host pre-allocates it). Each probe zeroes its
         * out-param before it can fail, so they land in locals and are copied
         * back only on success: a transient read error must not blank a good
         * boot-time value. The log's "next" LBA advances as blocks flush, so
         * the parked reading can lag by a few blocks — acceptable, since the
         * cross-check against make_log.py --verify is done with the device on
         * the cable and the drive up. */
        if (!ata_is_parked()) {
            uint32_t p0 = 0, p1 = 0;
            if (config_probe_lba(0, &p0) == 0) g_diag_cfg_lba[0] = p0;
            if (config_probe_lba(1, &p1) == 0) g_diag_cfg_lba[1] = p1;
            if (evlog_enabled()) {
                uint32_t h = 0, n = 0;
                if (evlog_probe_header_lba(&h) == 0)          g_diag_log_lba[0] = h;
                if (evlog_probe_lba(evlog_seq(), &n) == 0)    g_diag_log_lba[1] = n;
            }
        }
        const player_stats_t *ps = player_stats();
        settings_diag_render(g_boot_total_ms, g_boot_lcd_ms, g_boot_disk_ms,
                             g_lib_load_ms, g_boot_resume_ms,
                             g_boot_res_dir_ms, g_boot_res_open_ms,
                             g_boot_res_seek_ms,
                             ps ? ps->decode_us_per_kframe : 0,
                             ps ? ps->decode_rate : 0,
                             ps ? ps->underruns : 0,
                             config_writable(), config_seq(),
                             g_diag_cfg_lba[0], g_diag_cfg_lba[1],
                             g_diag_log_lba[0], g_diag_log_lba[1],
                             CORE_BUILD_ID);
    } else if (g_set_screen == SETTINGS_BATTERY) {
        /* The Battery page: everything on it but the Charge Rate row is a
         * live value, and this is the only place that has them. The pins
         * are read back per paint (six GPIO reads, no I2C) — the point of
         * the footer is that it shows the pad, not the request. The trend
         * comes from the record every 5 s sample feeds (chargestat). */
        charger_pins_t pins;
        charger_pin_state(&pins);
        static uint16_t hist[BATTERY_HIST_MAX];
        battery_page_t bp;
        uint32_t now = mmio_read32(USEC_TIMER_ADDR);
        bp.pct         = g_bat_pct;
        bp.mv          = g_bat_mv_filt;
        bp.adc         = g_bat_raw;
        bp.source      = power_source();
        bp.charging    = power_is_charging();
        bp.rate_ma     = charger_max_current();
        bp.hpwr        = pins.hpwr;
        bp.susp        = pins.susp;
        bp.hpwr_cfg    = (uint8_t)((pins.hpwr_gpio ? BATTERY_PIN_GPIO : 0) |
                                   (pins.hpwr_out  ? BATTERY_PIN_OUT  : 0));
        bp.susp_cfg    = (uint8_t)((pins.susp_gpio ? BATTERY_PIN_GPIO : 0) |
                                   (pins.susp_out  ? BATTERY_PIN_OUT  : 0));
        bp.session_s   = chargestat_session_s(&g_chg, now);
        bp.session_mv0 = chargestat_session_mv0(&g_chg);
        bp.session_dmv = chargestat_session_delta_mv(&g_chg);
        bp.session_chg = chargestat_session_chg_pct(&g_chg);
        bp.trend_ok    = chargestat_recent_delta(&g_chg, BATTERY_TREND_MIN,
                                                 &bp.trend_mv);
        if (!bp.trend_ok) bp.trend_mv = 0;
        bp.hist_n      = chargestat_history(&g_chg, hist);
        bp.hist        = hist;
        bp.playing     = player_active() && !player_paused();
        bp.parked      = ata_is_parked();
        settings_battery_render(&g_settings, g_set_sel, &bp);
    } else if (g_set_screen == SETTINGS_SETTIME) {
        settime_render(&g_settime);
    } else {
        settings_render(g_set_screen, &g_settings, g_set_sel);
    }
    status_strip_render();
}

/* Right-side circle arc (a ")" shape): radius R, +/- span rows tall, centred at
 * (cx, cy); ~2px thick. The speaker's sound waves + the lock shackle use it. */
/* A 1px-thin right-opening crescent (radius R, +/-span rows), anchored so its
 * near point sits by the speaker cone — the skinny sound wave. */
static void draw_arc_thin(int cx, int cy, int R, int span, uint16_t c)
{
    for (int dy = -span; dy <= span; dy++) {
        int dx = ui_isqrt(R * R - dy * dy);
        console_fill_rect(cx + dx, cy + dy, 1, 1, c);
    }
}

/* A small right-pointing speaker: driver cabinet + cone, then sound waves that
 * scale with `vol` — two waves loud, one wave quiet, and a mute "X" at 0. */
static void draw_speaker(int sx, int sy, uint16_t c, int vol)
{
    ui_round_rect(sx - 8, sy - 3, 4, 6, 1, c);          /* cabinet             */
    for (int dx = 0; dx <= 4; dx++) {                     /* cone, opening right */
        int half = 2 + dx;
        console_fill_rect(sx - 4 + dx, sy - half, 1, 2 * half, c);
    }
    if (vol <= 0) {                                       /* muted: an X         */
        for (int i = 0; i < 6; i++) {
            console_fill_rect(sx + 3 + i, sy - 3 + i, 2, 1, c);   /* '\'         */
            console_fill_rect(sx + 3 + i, sy + 2 - i, 2, 1, c);   /* '/'         */
        }
        return;
    }
    /* Three skinny (1px) crescent waves anchored at the cone front, growing with
     * volume — closer + thinner than the old 2px arcs (chosen design "B"). */
    if (vol > 5)  draw_arc_thin(sx, sy, 3, 2, c);         /* wave 1: hairline    */
    if (vol > 40) draw_arc_thin(sx, sy, 6, 4, c);         /* wave 2              */
    if (vol > 72) draw_arc_thin(sx, sy, 9, 5, c);         /* wave 3              */
}

/* Centered volume overlay plate (volume-demo.jsx VolumeOverlay): speaker glyph +
 * ink fill bar + big percent, on a light near-surface plate. Everything it
 * draws stays inside the VOL_PLATE_* rect (chrome.h) — the main loop presents
 * exactly that rect for a volume tick, so ink outside it would never reach
 * the panel.
 *
 * The bar keeps its 0..100 scale whatever the Volume Limit is, and a limit
 * below 100 puts a small triangle over the track at the ceiling — the marker
 * the 5G's own Features Guide describes, and the only thing on screen that
 * explains why the wheel stopped. */
static void volume_overlay_render(int vol, int limit)
{
    const int PX = VOL_PLATE_X, PY = VOL_PLATE_Y, PW = VOL_PLATE_W, PH = VOL_PLATE_H;
    ui_round_rect(PX, PY, PW, PH, 8, LINEN_PLATE);    /* raised plate, AA r8  */

    draw_speaker(PX + 16, PY + PH / 2, LINEN_INK, vol);

    /* Fill bar. */
    int bx = PX + 34, by = PY + PH / 2 - 3, bw = PW - 34 - 42, bh = 6;
    console_fill_rect(bx, by, bw, bh, LINEN_TRK);          /* track rgba(ink,0.12) */
    int fw = bw * vol / 100;
    if (fw < 0) fw = 0;
    if (fw > bw) fw = bw;
    console_fill_rect(bx, by, fw, bh, LINEN_INK);

    /* Ceiling marker: a 5x3 triangle, apex down, sitting ON the track's top
     * edge. Rows PY+9..PY+11 and x in [bx-2, bx+bw+2] are both well inside
     * the plate, so the present rect the caller pushes still covers every
     * pixel drawn here. The fill can never reach past it (vol <= limit by
     * construction), so the two never collide. */
    if (limit < 100) {
        int mx = bx + bw * limit / 100;
        console_fill_rect(mx - 2, by - 4, 5, 1, LINEN_INK);
        console_fill_rect(mx - 1, by - 3, 3, 1, LINEN_INK);
        console_fill_rect(mx,     by - 2, 1, 1, LINEN_INK);
    }

    /* Percent, right-aligned. */
    char p[5];
    u32_to_dec(p, (unsigned)vol);
    int w = text_width(p, text_font_bold_12());       /* percent is 11/700       */
    ui_text(PX + PW - 14 - w, PY + PH / 2 + 4, p, text_font_bold_12(), LINEN_INK);
}

/* Now-playing album art at 120x120 — the stored folder.art's native size, so it
 * blits straight through at full quality (no downscale) and fills the art-left
 * layout. Only art larger than this is box-downscaled (cached per track). */
#define NP_ART_DIM 120
static uint16_t g_np_art[NP_ART_DIM * NP_ART_DIM];
static int      g_np_art_key = -1;

static const uint16_t *np_art_120(void)
{
    if (!player_art_ok()) { g_np_art_key = -1; return 0; }
    int w = player_art_w(), h = player_art_h();
    if (w == NP_ART_DIM && h == NP_ART_DIM) {
        return player_art_pixels();        /* native size: blit as-is */
    }
    /* Keyed on the ART generation, not the queue index: a freshly built queue
     * that happens to start on the same index would otherwise re-use the
     * PREVIOUS album's downscaled cover. player_art_seq() bumps whenever the
     * art changes, including when it is cleared. */
    int key = (int)player_art_seq();
    if (key != g_np_art_key) {
        thumb_box_rgb565(player_art_pixels(), w, h,
                         g_np_art, NP_ART_DIM, NP_ART_DIM);
        g_np_art_key = key;
    }
    return g_np_art;
}

/* Now-playing TRANSPORT strip: the elapsed / −remaining times and the progress
 * bar, i.e. everything that changes once a second. Split out of the full
 * renderer because the rest of the screen (art + metadata) only changes on a
 * track change: the clock tick redraws THIS band and presents only it, instead
 * of clearing the framebuffer, re-blitting the 120x120 cover and rebuilding
 * every metadata string every second and discarding all of it above y=128. */
/* NP_TR_Y / NP_TR_H (the band's rect) live in ui/chrome.h so the host present
 * test can pin the word count this band costs on the BCM. */

static void nowplaying_transport_render(uint32_t elapsed_s, uint32_t total_s)
{
    console_fill_rect(0, NP_TR_Y, LCD_WIDTH, NP_TR_H, LINEN_SURFACE);

    /* While an aim is in flight — the wheel scrubber, or a RIGHT/LEFT hold —
     * the bar and the left-hand time show the TARGET, not the live position:
     * the gesture is aiming at a destination and the readout has to be what
     * you are aiming at. The right-hand side becomes the delta from where
     * playback actually is, so a long seek is legible ("+3:41") instead of a
     * remaining figure you have to subtract in your head. */
    uint32_t aim    = 0;
    int      aiming = np_aim_target(&aim);
    uint32_t shown  = aiming ? aim : elapsed_s;

    char te[FMT_TIME_MAX], tr[FMT_TIME_MAX + 2];   /* tr carries a sign prefix */
    fmt_time(te, shown);
    if (aiming) {
        uint32_t d = (shown > elapsed_s) ? shown - elapsed_s : elapsed_s - shown;
        tr[0] = (shown >= elapsed_s) ? '+' : '-';
        fmt_time(tr + 1, d);
    } else {
        uint32_t rem = (total_s > elapsed_s) ? total_s - elapsed_s : 0;
        tr[0] = '-';
        fmt_time(tr + 1, rem);                         /* "−M:SS" remaining     */
    }
    ui_text(18, 198, te, FONT_SUB, aiming ? LINEN_INK : LINEN_MUTED_D);
    int wtr = text_width(tr, FONT_SUB);
    ui_text(LCD_WIDTH - 18 - wtr, 198, tr, FONT_SUB,
            aiming ? LINEN_INK : LINEN_MUTED_D);

    /* Taller rounded-cap bar (INK fill on a faint ink track, Theme1Live fg).
     * bh 8 with AA pill caps reads smoother than the old 6px integer-stepped
     * caps. */
    int pbx = 18, by = 209, bw = LCD_WIDTH - 36, bh = 8;
    ui_round_rect(pbx, by, bw, bh, bh / 2, LINEN_TRK);
    int fw = (total_s > 0) ? (int)((shown * (uint32_t)bw) / total_s) : 0;
    if (fw > bw) fw = bw;
    if (fw >= bh) {
        ui_round_rect(pbx, by, fw, bh, bh / 2, LINEN_INK);
    } else if (fw > 0) {
        console_fill_rect(pbx, by, fw, bh, LINEN_INK);
    }
    /* A playhead at the target so the aim point is readable even where the
     * filled length is ambiguous (very short or very long tracks). */
    if (aiming) {
        int hx = pbx + fw - 1;
        if (hx < pbx)          hx = pbx;
        if (hx > pbx + bw - 3) hx = pbx + bw - 3;
        console_fill_rect(hx, by - 3, 3, bh + 6, LINEN_INK);
    }
}

/* Now-playing (Linen, interactive-ipod.jsx Theme1Live): a top status row
 * ("Now Playing"/"Paused" + shuffle/repeat + battery), then 88x88 art on the
 * LEFT with a metadata column to its right (SONG N OF M / title / artist /
 * album), and the elapsed / −remaining times + progress bar pinned to the
 * bottom. Art + metadata come from the PLAYING folder, not the browsed one. */
static void nowplaying_render(const char *name, uint32_t elapsed_s,
                              uint32_t total_s, uint32_t buf_pct)
{
    (void)buf_pct;
    console_clear(LINEN_SURFACE);

    /* --- top status row: context label left, state + battery right --------- */
    ui_text(12, 15, player_paused() ? "Paused" : "Now Playing",
            text_font_bold_12(), LINEN_INK);

    int bx = LCD_WIDTH - 12 - 19;                     /* battery block         */
    draw_battery(bx, 3, g_bat_pct);
    int rc = bx - (g_locked ? 18 : 6);                /* right edge for tokens */
    if (g_locked) {
        draw_lock_glyph(bx - 14, 3, LINEN_INK);
    }
    /* Compact shuffle / repeat / sleep tokens, right-aligned before the
     * battery. Built as ONE string and measured once, so the spacing between
     * them is the font's own kerning rather than three guessed offsets. Worst
     * case "SHUF·ALB RPT1 SLEEP 120" is 128 px against the ~194 px of air this
     * band has beside "Now Playing" (23 bytes into st[32], too). */
    {
        char st[32]; int p = 0;
        if (g_settings.shuffle) {
            /* SHUF·ALB only when albums is what is actually being walked: a
             * Shuffle Songs queue IS its own song order (PLAYER_KEEP_QUEUE),
             * and the setting does not regroup it, so the token must not
             * claim otherwise. */
            const char *s = (g_settings.shuffle == SHUFFLE_ALBUMS &&
                             player_order_keep() != PLAYER_KEEP_QUEUE)
                              ? "SHUF" UI_GLYPH_MIDDOT "ALB" : "SHUF";
            while (*s) st[p++] = *s++;
        }
        if (g_settings.repeat != REPEAT_OFF) {
            if (p) st[p++] = ' ';
            const char *s = (g_settings.repeat == REPEAT_ONE) ? "RPT1" : "RPT";
            while (*s) st[p++] = *s++;
        }
        {
            char tok[12];                    /* "" while the timer is off */
            if (sleeptimer_token(&g_sleep, tok, (int)sizeof tok) > 0) {
                if (p) st[p++] = ' ';
                const char *s = tok; while (*s) st[p++] = *s++;
            }
        }
        st[p] = '\0';
        if (p) {
            int w = text_width(st, FONT_SMALL);
            ui_text(rc - w, 13, st, FONT_SMALL, LINEN_MUTED2);
        }
    }

    /* --- art on the left (native 120x120) ---------------------------------- */
    const int ax = 16, ay = 44;
    const uint16_t *art = np_art_120();
    if (art) {
        console_blit565(ax, ay, NP_ART_DIM, NP_ART_DIM, art);
    } else {
        /* No cover: a neutral tile, same as the list rows' chip placeholder —
         * bare surface here read as a rendering bug, not as "no artwork". */
        console_fill_rect(ax, ay, NP_ART_DIM, NP_ART_DIM, LINEN_BORDER);
    }

    /* --- metadata column, vertically centred beside the taller art --------- */
    const flac_meta_t *m = player_meta();
    const char *title = (m->have && m->title[0]) ? m->title : track_display(name);
    int mx = ax + NP_ART_DIM + 14;                    /* metadata left edge    */
    int mr = LCD_WIDTH - 14;                           /* metadata right edge   */

    /* "TRACK N OF M" eyebrow above the title (design's "Track 04 of 11"). */
    int tot = player_queue_len();
    if (tot > 0) {
        /* 8 + 12 literal chars + two decimals (<=10 digits each) + NUL. The old
         * 24-byte buffer only covered ONE-digit numbers on both sides: any album
         * with >=10 tracks overran it, and a 1200-entry shuffle queue by 5. */
        char num[8 + 12 + 10 + 10 + 1];
        int p = 0;
        for (const char *q = "TRACK   "; *q; q++) num[p++] = *q;   /* air after TRACK */
        p += u32_to_dec(num + p, (unsigned)(player_queue_current() + 1));
        for (const char *q = "     OF     "; *q; q++) num[p++] = *q;  /* extra air both sides */
        p += u32_to_dec(num + p, (unsigned)tot);
        num[p] = '\0';
        ui_text(mx, 72, num, FONT_SMALL, LINEN_MUTED2);
    }

    /* Title (bold 17): drawn at its live scroll offset + registered as the
     * marquee, so volume/lock full repaints never flash the truncated title. */
    mq_text(mx, 94, mr - mx, title, FONT_TITLE, LINEN_INK, LINEN_SURFACE,
            0, LCD_HEIGHT);   /* now-playing title has room — no row clip */

    if (m->have && m->artist[0]) {
        ui_text_clip(mx, 114, m->artist, FONT_SUB, LINEN_MUTED_D, mx, mr);
    }
    if (m->have && m->album[0]) {
        ui_text_clip(mx, 130, m->album, FONT_SUB, LINEN_MUTED2, mx, mr);
    }

    nowplaying_transport_render(elapsed_s, total_s);

    /* Volume overlay rides on top for ~1.5 s after a wheel adjustment. */
    if (ui_window_up(&g_vol_show, VOL_SHOW_US, mmio_read32(USEC_TIMER_ADDR))) {
        volume_overlay_render(g_volume, g_settings.volume_limit);
    }
}

/* ---------------------------------------------------------------------------
 * Menus (main + Music sub-menu)
 *
 * Both are the same widget over a small {label, active} item list. Inactive
 * items render greyed and SELECT does nothing (features not yet built). The
 * renderer reuses the list row geometry (LIST_Y0/ROW_H) and, since Search
 * took Music past LIST_ROWS, the same scroll window and scrollbar every other
 * list has. A menu that silently drops its ninth row is the worst way to find
 * out it grew.
 * ------------------------------------------------------------------------- */
typedef struct { const char *label; uint8_t active; } menu_item_t;

/* Main menu. ACTIVE: Music, Now Playing (Now Playing greyed until something is
 * playing — its `active` flag is refreshed from the player before each paint). */
enum { MM_MUSIC, MM_PLAYLISTS, MM_PODCASTS, MM_AUDIOBOOKS, MM_SETTINGS,
       MM_NOWPLAYING, MM_COUNT };
static menu_item_t g_main_menu[MM_COUNT] = {
    { "Music",       1 },
    { "Playlists",   1 },
    { "Podcasts",    0 },
    { "Audiobooks",  0 },
    { "Settings",    1 },
    { "Now Playing", 1 },
};
static int g_main_sel;

/* Music sub-menu. ACTIVE: Playlists, Artists, Albums, Songs, Shuffle Songs,
 * Genres; Composers and Audiobooks are greyed (no backing implementation). */
enum { MU_PLAYLISTS, MU_ARTISTS, MU_ALBUMS, MU_SONGS, MU_SHUFFLE, MU_GENRES,
       MU_SEARCH, MU_COMPOSERS, MU_AUDIOBOOKS, MU_COUNT };
static const menu_item_t g_music_menu[MU_COUNT] = {
    { "Playlists",     1 },
    { "Artists",       1 },
    { "Albums",        1 },
    { "Songs",         1 },
    { "Shuffle Songs", 1 },
    { "Genres",        1 },
    { "Search",        1 },
    { "Composers",     0 },
    { "Audiobooks",    0 },
};
static int g_music_sel;

/* Shared wheel accumulator for the menu screens. */
int g_menu_accum;

/* Generic menu renderer over a {label, active} list. `back` draws the header
 * back chevron (off for the root menu). */
/* The item list + selection the menu rows are drawn from — set by
 * menu_render_list (and by list_view_current, for the partial repaint path)
 * so menu_row_draw has the same row-drawing signature as every other list. */
static const menu_item_t *g_menu_items;
static int                g_menu_sel;

static void menu_row_draw(int r, int idx)
{
    list_row(r, g_menu_items[idx].label, 0, 0, 1 /*chevron*/, idx == g_menu_sel,
             !g_menu_items[idx].active /*greyed*/, 0);
}

static void menu_render_list(const char *title, const char *right,
                             const menu_item_t *items,
                             int n, int sel, int back)
{
    g_menu_items = items;
    g_menu_sel   = sel;
    console_clear(LINEN_SURFACE);
    status_strip_render();
    ui_header(title, right ? right : "", back);
    int top = ui_scroll_window(sel, n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= n) break;
        menu_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, n);
}

/* Visible main-menu row count: "Now Playing" (the last item) only appears while
 * a track is loaded, so it's simply dropped from the count when idle. */
static int main_menu_count(void)
{
    return player_active() ? MM_COUNT : MM_COUNT - 1;
}

static void main_menu_render(void)
{
    int n = main_menu_count();
    if (g_main_sel >= n) g_main_sel = n - 1;   /* cursor was on a now-gone row */
    /* The clock lives in the main menu's header slot, which is empty today and
     * which ui_header measures BEFORE the title — so "Core" (30 px) and a
     * clock cannot collide however wide the clock gets. This is the one place
     * the time is shown while a track is playing: the strip keeps the track
     * name (strip_left_text). */
    char clock[DATETIME_TIME_MAX];
    clock_title_text(clock, (int)sizeof clock);
    menu_render_list("Core", clock, g_main_menu, n, g_main_sel, 0);
}

static void music_menu_render(void)
{
    menu_render_list("Music", NULL, g_music_menu, MU_COUNT, g_music_sel, 1);
}

/*
 * Leaving Settings: the bookkeeping that MENU does on the way out, without
 * the pop itself. Extracted so the MENU tap and the MENU hold (which jumps
 * straight to the main menu from anywhere) run exactly the same exit — a
 * change made in Settings has to reach the platter either way.
 *
 * Resume may have just been switched OFF, and dropping the stored locator is
 * part of honouring that; doing it here folds the clear into the commit
 * instead of costing a second write when the interval next comes round.
 *
 * SOFT, not FORCE. resume_capture() marks the record dirty whenever a track
 * is loaded (the position moved), so a FORCE here meant that leaving Settings
 * with the drive PARKED paid a 1-3 s ata_wakeup() spin-up before the pop was
 * even rendered — the stall was the back-out itself, every time. SOFT writes
 * when the platters are already turning and otherwise leaves the change
 * pending for the idle path or the next forced commit (suspend / power-off /
 * disk mode).
 */
void settings_leave(void)
{
    g_set_editing = 0;
    resume_capture();
    settings_commit(CFG_COMMIT_SOFT);
    otg_commit(CFG_COMMIT_SOFT);          /* a list built before Settings, too */
}

/*
 * Settings > Theme previews the theme under the cursor: the picker page is
 * painted in it as the wheel moves, and nothing in g_settings changes, so
 * nothing is written. SELECT applies for real (settings_activate ->
 * settings_apply); every other way out — MENU back to the Settings root, the
 * MENU hold to the main menu, a modal pushed over it, a suspend — lands on a
 * screen where settings_theme_shown() answers the saved theme, and the next
 * pass puts it back. Called once per main-loop pass, after input and before
 * the repaint; returns 1 when it swapped the palette (the caller repaints the
 * whole panel). theme_current() makes the steady state free: no copy unless
 * the wanted theme changed.
 *
 * The chip placeholders are deliberately NOT re-baked for a preview: no
 * Settings screen draws an album chip, and the only screens that do are
 * reached through a path where the saved theme is already back.
 */
static int theme_preview_sync(void)
{
    int want = (scr_cur() == SCR_SETTINGS)
                   ? settings_theme_shown(g_set_screen, g_set_sel, &g_settings)
                   : g_settings.theme;
    if (want == theme_current()) return 0;
    theme_set(want);
    return 1;
}

/* Why the last browse_load produced what it did: 0, or the FAT32_* code of a
 * directory read that failed even after retries. An empty g_browse with a
 * nonzero code is an album that COULD NOT BE READ, not an album with no
 * tracks; the tracklist screen should say so (and offer Back), because the
 * two look identical otherwise and one of them is a disk fault. */
static int g_browse_err;

/*
 * Which library song each g_browse row IS — a g_songs index, or -1 for a file
 * the library has no record of — and the row's ordering key. Both filled by
 * browse_bind() at the end of every browse_load, so the tracklist screen,
 * the album queue and the resume path all read the same answer.
 *
 * Rows bind to songs by FILE CLUSTER: the resolve pass already bound each
 * record to its directory entry (by file_hash), and the row was made from
 * that same entry, so the cluster is the one fact both sides hold exactly.
 * Matching by name was what this replaces — the row's ext-trimmed name
 * against the record's file[] field, which for a long filename is a
 * truncated string no row could equal, so those rows showed no duration,
 * no title and a made-up gutter number while the same file played fine.
 */
static int16_t  g_browse_song[BROWSE_MAX];
static uint32_t g_browse_key[BROWSE_MAX];

static int browse_key_cmp_idx(uint16_t a, uint16_t b)
{
    uint32_t ka = g_browse_key[a], kb = g_browse_key[b];
    return (ka > kb) - (ka < kb);
}

static void browse_bind(uint32_t dir_clus)
{
    int n = g_browse_n;
    for (int i = 0; i < n; i++) {
        g_browse_song[i] = -1;
        g_browse_key[i]  = 0xFFFFFFFFu;       /* unbound rows sort last */
        g_browse[i].order_key = 0xFFFFFFFFu;  /* the row carries it into the
                                               * queue (Shuffle Albums)      */
    }
    /* One pass over the library: the album's songs are the ones whose folder
     * is this one, and each finds its row by cluster among at most BROWSE_MAX.
     * ~6000 integer compares plus a few hundred per album open — an event
     * that just read a directory off the disk. */
    for (int s = 0; s < g_songs_n; s++) {
        const lib_song_t *sg = &g_songs[s];
        if (sg->dir_clus != dir_clus || sg->file_clus == 0) continue;
        for (int i = 0; i < n; i++) {
            if (g_browse[i].is_dir || g_browse[i].clus != sg->file_clus) continue;
            g_browse_song[i] = (int16_t)s;
            g_browse_key[i]  = ((uint32_t)sg->disc << 16) | sg->track;
            /* Written onto the row as well as into the sort key: the album
             * queue is a copy of g_browse, and Shuffle Albums orders a group
             * by exactly this. The swap loop below carries it along with the
             * rest of the entry. */
            g_browse[i].order_key = g_browse_key[i];
            break;
        }
    }

    /*
     * Order the rows by the index's (disc, track). They arrive in raw FAT
     * directory order — whatever order the importer happened to copy the
     * files in — while the gutter prints the record's track number, so an
     * album whose numbers did not come from the filenames read 7, 2, 11 down
     * the screen. One authority: the index. Ties, and rows the index does not
     * know, keep directory order (the sort is stable; unbound keys are max).
     * The queue is built from g_browse in this order too, so the album PLAYS
     * in it as well as listing in it.
     *
     * Applied in place by following cycles, inverted first — the same steps
     * as library_finish, for the same reason (the sort yields a gather order
     * and the swap loop scatters).
     */
    static uint16_t order[BROWSE_MAX];
    for (int i = 0; i < n; i++) order[i] = (uint16_t)i;
    merge_sort_idx(order, n, g_sort_tmp, browse_key_cmp_idx);
    uint16_t *inv = g_sort_tmp;             /* free again once the sort is done */
    for (int k = 0; k < n; k++) inv[order[k]] = (uint16_t)k;
    for (int i = 0; i < n; i++) {
        while (inv[i] != (uint16_t)i) {
            int j = inv[i];
            browse_entry_t te = g_browse[i];
            g_browse[i] = g_browse[j];
            g_browse[j] = te;
            int16_t ts = g_browse_song[i];
            g_browse_song[i] = g_browse_song[j];
            g_browse_song[j] = ts;
            uint32_t tk = g_browse_key[i];
            g_browse_key[i] = g_browse_key[j];
            g_browse_key[j] = tk;
            uint16_t ti = inv[i];
            inv[i] = inv[j];
            inv[j] = ti;
        }
    }
}

/* Read an album's tracklist (the folder at `dir_clus`) into g_browse, bound to
 * the library and in the index's order (browse_bind). Only ever called at
 * depth 1 now — the album LIST is the index-driven g_albums. */
static void browse_load(fat32_t *fs, uint32_t dir_clus)
{
    g_list_epoch++;
    g_cur_dir  = dir_clus;
    g_browse_n = 0;
    g_art_clus = 0;                      /* re-captured by browse_collect below */
    g_art_size = 0;
    /* One directory, opened by hand: give it the full set of retries rather
     * than whatever the boot load left in the budget — the budget exists to
     * bound a thousand-album boot, not a single user-initiated open. */
    g_lib_retry_budget = LIB_READDIR_RETRIES;
    g_browse_err = lib_readdir(fs, dir_clus, browse_collect, 0, &g_browse_n);
    if (g_browse_err != 0) {
        g_art_clus = g_art_size = 0;     /* nothing from a walk we don't trust */
        return;
    }
    /* The folder just read cleanly. If the load-time pass could not read it,
     * this is the re-attempt the flag promised: resolve it now, so its songs
     * become playable from Songs/Genres/shuffle and the About count drops.
     * Before the bind, which needs the clusters the resolve installs. */
    int ai = album_by_clus(dir_clus);
    if (ai >= 0 && g_albums[ai].unreadable) (void)album_resolve(fs, ai);
    browse_bind(dir_clus);
}

/* After entering an album folder: load its hero art, pull each track's
 * disc/duration/number from the index (matched by folder+filename), and build
 * the display view with "Disc N" section headers for a multi-disc album. */
static void detail_load_meta(fat32_t *fs)
{
    detail_art_load(fs);
    g_album_track_n = 0;
    int maxd = 0;
    for (int i = 0; i < g_browse_n; i++) {
        g_track_dur[i] = 0;
        g_track_disc[i] = 0;
        g_track_num[i] = 0;
        g_track_title[i] = 0;
        if (g_browse[i].is_dir) continue;
        g_album_track_n++;
        /* The row's song, bound by cluster in browse_bind. No longer gated on
         * "loaded from the index": a scanned library binds the same way and
         * has durations and numbers of its own to show. */
        int s = g_browse_song[i];
        if (s >= 0) {
            g_track_dur[i]  = (uint16_t)g_songs[s].duration_s;
            g_track_disc[i] = (uint8_t)g_songs[s].disc;
            g_track_num[i]  = g_songs[s].track;
            if (g_songs[s].title[0]) g_track_title[i] = g_songs[s].title;
            if (g_songs[s].disc > maxd) maxd = g_songs[s].disc;
        }
    }
    g_detail_multidisc = (maxd > 1);
    /* Sum the runtime once, here — detail_render used to re-add every track's
     * duration on every paint just to print one meta line. */
    g_detail_total_s = 0;
    for (int i = 0; i < g_browse_n; i++) g_detail_total_s += g_track_dur[i];
    g_det_view_n = 0;
    int prev = -1;
    for (int i = 0; i < g_browse_n && g_det_view_n < DET_VIEW_MAX - 1; i++) {
        if (g_browse[i].is_dir) continue;
        int d = g_track_disc[i];
        if (g_detail_multidisc && d > 0 && d != prev) {
            g_det_view[g_det_view_n++] = (int16_t)(-(d + 1));   /* Disc header */
            prev = d;
        }
        g_det_view[g_det_view_n++] = (int16_t)i;
    }
}

/*
 * PLAY tapped: start what is highlighted, if what is highlighted names music.
 *
 * On a real iPod, Play on an album, an artist, a genre, a playlist or a song
 * PLAYS it; it only means pause/resume where there is no such row under the
 * cursor. That is a per-screen policy, so it lives in ui/gesture.c where the
 * host can pin every screen's answer (gesture_play_tap); this function is the
 * other half — the mapping from the live screen to a context, and then the
 * same builder SELECT would have called.
 *
 * A player that is already going is REPLACED: the builders stop the old track
 * and begin a new queue, which is the rule the gesture encodes ("Play on a
 * list title plays that list"). Tapping Play on the album already playing
 * restarts it from track 1, exactly as the original does.
 */
/*
 * PLAY_TAP_CONSUMED is the tap that was acted on and started nothing: a
 * playlist whose file cannot be read or whose tracks have all gone missing
 * (its tracklist screen is now up carrying the reason), an album folder that
 * could not be read (the same), or a view whose every song has vanished off
 * the disk since the index was built. That last one is the reason this is not
 * simply PASS: library_play_song() and shuffle_songs_build() clear the old
 * queue BEFORE they discover there is nothing to add, so the music has
 * already stopped and the status strip's track name has to repaint away. In
 * all of them the screen changed, and PLAY must not ALSO toggle a transport
 * that is no longer there.
 */
typedef enum {
    PLAY_TAP_PASS = 0,   /* not a title: PLAY still means pause/resume        */
    PLAY_TAP_STARTED,    /* a queue started and Now Playing is up             */
    PLAY_TAP_CONSUMED,   /* acted on, started nothing: repaint, do not toggle */
} play_tap_t;

/* Defined with the rest of Music > Search, which needs the album and playlist
 * loaders and so lands below this. */
static play_tap_t search_play_hit(fat32_t *fs);

static play_tap_t play_tap_start(fat32_t *fs)
{
    gesture_ctx_t ctx;
    int           count;

    switch (scr_cur()) {
    case SCR_MUSIC:
        /* Shuffle Songs is the one menu row that names a queue; the count it
         * needs is the library's, not the menu's. */
        ctx   = (g_music_sel == MU_SHUFFLE) ? GESTURE_CTX_MUSIC_SHUFFLE
                                            : GESTURE_CTX_MUSIC_OTHER;
        count = g_songs_n;
        break;
    case SCR_ARTISTS:   ctx = GESTURE_CTX_LIST_TITLE; count = g_artists_n;   break;
    case SCR_GENRES:    ctx = GESTURE_CTX_LIST_TITLE; count = g_genres_n;    break;
    case SCR_PLAYLISTS: ctx = GESTURE_CTX_LIST_TITLE;
                        count = playlists_row_count();                       break;
    case SCR_SONGS:     ctx = GESTURE_CTX_LIST_TRACK; count = g_songview_n;  break;
    case SCR_SEARCH: {
        /* The picker is a text field: nothing is highlighted, so PLAY stays
         * the transport it is everywhere else — a count of 0 is exactly how
         * gesture_play_tap is told that. In RESULTS every row names music,
         * and which KIND it is decides the queue, as it does on the list the
         * row came from. ui/search.c owns that judgement. */
        int is_track = 0;
        count = search_play_rows(&g_search, &is_track);
        ctx   = is_track ? GESTURE_CTX_LIST_TRACK : GESTURE_CTX_LIST_TITLE;
        break;
    }
    case SCR_PLAYLIST:  ctx = GESTURE_CTX_LIST_TRACK; count = g_pl_tracks_n; break;
    /* On-The-Go is a list of tracks with two action rows on top; PLAY on an
     * action row plays the list from its first track, which is what PLAY on
     * any list title does. */
    case SCR_OTG:       ctx = GESTURE_CTX_LIST_TRACK; count = otg_row_count(); break;
    case SCR_BROWSER:
        /* Depth 0 is the album list (rows that NAME a queue, plus the
         * artist's synthetic All Songs row); depth 1 is one album's tracks. */
        ctx   = (g_dir_depth == 0) ? GESTURE_CTX_LIST_TITLE
                                   : GESTURE_CTX_LIST_TRACK;
        count = (g_dir_depth == 0) ? albumlist_count() : g_browse_n;
        break;
    case SCR_NOWPLAYING:
    case SCR_QUEUE:     ctx = GESTURE_CTX_PLAYER;   count = 0; break;
    case SCR_SETTINGS:  ctx = GESTURE_CTX_SETTINGS; count = 0; break;
    case SCR_BATTERY:
    case SCR_CHARGING:  ctx = GESTURE_CTX_MODAL;    count = 0; break;
    case SCR_MENU:
    default:            ctx = GESTURE_CTX_MENU;     count = 0; break;
    }

    if (gesture_play_tap(ctx, count) != GESTURE_PLAY_START) {
        return PLAY_TAP_PASS;
    }

    switch (scr_cur()) {
    case SCR_MUSIC:                                  /* Shuffle Songs */
        shuffle_songs_play(fs);
        /* g_songs_n > 0 got us here, so a deal that produced nothing means
         * every song in the library is unresolved on disk — and the build has
         * already stopped whatever was playing. */
        if (!player_active()) return PLAY_TAP_CONSUMED;
        break;

    case SCR_ARTISTS: {
        /* Straight to the artist's whole discography — no album list in
         * between, because PLAY says "play", not "show me". */
        int k = 0;
        for (; g_artists[g_artist_sel].name[k] && k < NAME_MAX; k++) {
            g_artist_filter[k] = g_artists[g_artist_sel].name[k];
        }
        g_artist_filter[k] = '\0';
        songview_build(-1, g_artist_filter);
        if (library_play_song(fs, 0) < 0) return PLAY_TAP_CONSUMED;
        break;
    }

    case SCR_GENRES:
        songview_build(g_genre_sel, 0);
        if (library_play_song(fs, 0) < 0) return PLAY_TAP_CONSUMED;
        break;

    case SCR_SONGS:
        if (library_play_song(fs, g_song_sel) < 0) return PLAY_TAP_CONSUMED;
        break;

    case SCR_PLAYLISTS:
        if (g_pl_sel == 0) {              /* the pinned On-The-Go row */
            if (otg_play(0) < 0) return PLAY_TAP_PASS;
            break;
        }
        playlist_open(fs, g_pl_sel - 1);
        if (playlist_play(0) < 0) {
            /* Unreadable, or every file it names is gone. Show the tracklist
             * screen with the reason SELECT would have shown rather than
             * silently pausing whatever was playing. */
            scr_push(SCR_PLAYLIST);
            return PLAY_TAP_CONSUMED;
        }
        break;

    case SCR_PLAYLIST:
        /* Unreachable with rows on screen, and it cannot have torn the old
         * queue down: playlist_play() refuses before player_queue_begin().
         * A saved slot's Delete row is not a track; playlist_play() starts
         * from the top for it. */
        if (playlist_play(g_plt_sel) < 0) return PLAY_TAP_PASS;
        break;

    case SCR_OTG:
        if (otg_play(g_otg_sel >= OTG_ROW_FIRST ? g_otg_sel - OTG_ROW_FIRST : 0) < 0) {
            return PLAY_TAP_PASS;         /* nothing in it resolves to a file */
        }
        break;

    case SCR_SEARCH: {
        play_tap_t r = search_play_hit(fs);
        if (r != PLAY_TAP_STARTED) return r;
        break;                         /* the shared tail pushes Now Playing */
    }

    case SCR_BROWSER:
        if (g_dir_depth != 0) {                      /* a track in an album */
            g_queue_kind = RESUME_KIND_ALBUM;
            g_queue_seed = 0;
            player_play_queue(g_browse, g_browse_n, g_det_sel,
                              g_art_clus, g_art_size);
        } else if (albumlist_album_at(g_br_sel) < 0) {
            songview_build(-1, g_artist_filter);     /* the All Songs row   */
            if (library_play_song(fs, 0) < 0) return PLAY_TAP_CONSUMED;
        } else {
            /* Read the album's tracklist to build the queue from, without
             * ENTERING it: browse_collect only lists files at depth 1, so
             * borrow the depth for the read the way resume_open_album does
             * and hand it straight back. MENU from Now Playing then lands on
             * the album row that was pressed, not inside the album. */
            lib_album_t *al = &g_albums[albumlist_album_at(g_br_sel)];
            split_artist_album(al->folder, g_album_artist, g_album_title);
            g_dir_depth = 1;
            browse_load(fs, al->clus);
            detail_load_meta(fs);
            g_det_sel = g_det_accum = 0;
            if (g_browse_err != 0 || g_browse_n == 0) {
                /* Keep the depth: the tracklist screen is now up and says
                 * whether the folder could not be read or is simply empty —
                 * what SELECT would have shown. */
                return PLAY_TAP_CONSUMED;
            }
            g_queue_kind = RESUME_KIND_ALBUM;
            g_queue_seed = 0;
            player_play_queue(g_browse, g_browse_n, 0, g_art_clus, g_art_size);
            g_dir_depth = 0;
        }
        break;

    default:
        return PLAY_TAP_PASS;          /* gesture_play_tap starts nothing else */
    }

    hal_volume_set(g_volume);          /* re-apply over the codec re-init */
    scr_push(SCR_NOWPLAYING);
    return PLAY_TAP_STARTED;
}

/* ---------------------------------------------------------------------------
 * On-The-Go: the actions that move between screens, and the row SELECT
 * arbitration they hang off. Below play_tap_start because they push and pop
 * screens and call the same builders it does.
 * ------------------------------------------------------------------------- */

/*
 * Save the live list into the lowest free slot.
 *
 * A row that cannot be NAMED — its folder is not in the library root, its long
 * name is lossy, the path would be over M3U_PATH_MAX — is counted, never
 * silently dropped, so the banner can say "Saved 10 of 12" instead of the user
 * finding out on a train.
 */
static int otg_save_to_slot(fat32_t *fs)
{
    int n = otg_slot_free_index(fs);
    if (n == 0) {
        otg_flash(0, "No free saved list", 0);
        return 0;
    }
    uint32_t clus = 0, size = 0;
    if (!otg_slot_file(fs, n, &clus, &size)) {
        otg_flash(0, "Could not save", 0);
        return 0;
    }

    /* The first Save of a session prints the address it is about to write,
     * for the same reason the boot line prints COREOTG.DAT's: it has to be
     * checked against tools/make_otg.py --verify before the first write on a
     * device. Once per session — this is a blocking UART line. */
    if (!g_otg_slot_lba_said) {
        uint32_t lba = 0, run = 0;
        if (fat32_file_lba(fs, clus, &lba, &run) == 0) {
            uart_puts("core: otg slot ");
            uart_dec(n);
            uart_puts(" lba ");
            uart_put_hex32(lba);
            uart_putc('\n');
        }
        g_otg_slot_lba_said = 1;
    }

    /*
     * Shown unconditionally, not through load_bar_progress's "only if it turns
     * out to be slow" gate: this job has no progress to report (the writer
     * does not call back) and it is never fast — the whole 128 KiB file is
     * rewritten, plus one directory walk per album to read the exact on-disk
     * names. A frozen screen for a second or two is worse than one frame of
     * load bar.
     */
    load_bar("SAVING PLAYLIST", 0);
    int rows = 0;
    for (int i = 0; i < (int)g_otg.n; i++) {
        int si = g_otg_song[i];
        if (si < 0) continue;            /* names no file: nothing to write */
        g_otg_rows[rows].dir_clus  = g_songs[si].dir_clus;
        g_otg_rows[rows].file_clus = g_songs[si].file_clus;
        rows++;
    }
    /* The folder every row's dir_clus is looked up in, and the absolute
     * prefix that matches it: Music/ on the documented layout, the volume
     * root on the back-compat one — the same pair playlists_base_dir()
     * expresses for the read side. */
    int rc = otg_slot_save(fs, clus, size, playlists_root_clus(fs),
                           g_lib_root_clus ? "/Music/" : "/",
                           g_otg_rows, rows, g_otg.gen,
                           ata_write_sectors, &g_otg_save_scr, &g_otg_save_stats);
    uart_puts("core: otg slot save rc ");
    uart_put_hex32((uint32_t)rc);
    uart_puts(" wrote ");
    uart_dec((int)g_otg_save_stats.written);
    uart_puts(" of ");
    uart_dec((int)g_otg.n);
    uart_putc('\n');
    if (rc != 0) {
        otg_flash(0, "Could not save", 0);
        /* As a failed delete: the slot may be half-written, so read the
         * Playlists folder afresh on the next entry. */
        g_playlists_scanned = 0;
        return 1;                        /* the screen changed: repaint */
    }

    if (g_otg_save_stats.written == 0) {
        /* Every row was unsaveable. The slot now holds an empty list (which
         * is what it held before), and the live list must NOT be thrown away
         * for a save that saved nothing. */
        otg_flash(0, "Nothing could be saved", 0);
        return 1;
    }

    char label[40];
    int  k = 0;
    if (g_otg_save_stats.written == g_otg.n) {
        otg_str_copy(label, sizeof label, "Saved as On-The-Go ");
        k = 19;
    } else {
        otg_str_copy(label, sizeof label, "Saved ");
        k  = 6;
        k += u32_to_dec(label + k, g_otg_save_stats.written);
        otg_str_copy(label + k, sizeof label - (uint32_t)k, " of ");
        k += 4;
        k += u32_to_dec(label + k, g_otg.n);
        otg_str_copy(label + k, sizeof label - (uint32_t)k, " as On-The-Go ");
        k += 14;
    }
    label[k++] = (char)('0' + n);
    label[k]   = '\0';
    char tok[16];
    otg_count_token(tok, (int)g_otg_save_stats.written);
    otg_flash(+1, label, tok);

    /*
     * The list that was just saved may be the one PLAYING. The player holds
     * its own copy of the queue, so playback carries on either way — but the
     * resume record must now name the file, not a live list that is about to
     * be emptied. Re-point it; the next capture writes the playlist kind.
     */
    if (g_queue_kind == RESUME_KIND_OTG) {
        char slot[OTG_SLOT_NAME_BYTES];
        otg_slot_name(slot, n);
        g_queue_kind     = RESUME_KIND_PLAYLIST;
        g_queue_ctx_hash = name_hash(slot);
    }

    otg_clear(&g_otg);
    otg_bind_all();
    otg_touch();
    /* The moment is right for the write and the drive is certainly spinning
     * (we just wrote 128 KiB through it), so don't leave the emptied list to
     * the idle debounce. */
    otg_commit(CFG_COMMIT_SOFT);

    /* Show the saved list where it now lives. playlists_load re-scans, so the
     * new row is there and the emptied slot the save came from is gone. */
    playlists_load(fs);
    char want[OTG_SLOT_NAME_BYTES];
    otg_slot_name(want, n);
    for (int i = 0; i < g_playlists_n; i++) {
        if (name_eq_ci(g_playlists[i].name, want)) { g_pl_sel = i + 1; break; }
    }
    scr_pop();                           /* On-The-Go -> Playlists */
    return 1;
}

/* Delete Playlist on a saved slot: the empty form over the same file, which is
 * what makes the slot free again. */
static int otg_delete_slot(fat32_t *fs)
{
    if (g_pl_open < 0 || g_pl_slot <= 0) {
        return 0;
    }
    load_bar("DELETING PLAYLIST", 0);    /* the same 128 KiB rewrite */
    int rc = otg_slot_save(fs, g_playlists[g_pl_open].clus,
                           g_playlists[g_pl_open].size, 0, "/",
                           0, 0, g_otg.gen,
                           ata_write_sectors, &g_otg_save_scr, &g_otg_save_stats);
    uart_puts("core: otg slot erase rc ");
    uart_put_hex32((uint32_t)rc);
    uart_putc('\n');
    if (rc != 0) {
        otg_flash(0, "Could not delete", 0);
        /* The rewrite may have stopped partway: read the folder afresh on
         * the next entry rather than trust the list from before. */
        g_playlists_scanned = 0;
        return 1;
    }
    otg_flash(-1, "Deleted", 0);
    playlists_load(fs);
    scr_pop();                           /* the tracklist -> Playlists */
    return 1;
}


/* ---------------------------------------------------------------------------
 * SELECT on a list row: tap or hold.
 *
 * Every row that can be ADDED to On-The-Go is a row SELECT already did
 * something to, so the two have to be told apart by press length — which is
 * only known on the release or at the threshold. That is exactly the machine
 * ui/keyhold.c is, and PLAY, MENU and RIGHT/LEFT already go through it; this
 * is the fifth button and the first one whose meaning also depends on WHICH
 * ROW was under the thumb when the press began.
 *
 * So the down-edge records the row and arms the machine, and a per-pass block
 * in the main loop decides. Only the four screens with a hold action do this
 * (Songs, both halves of the browser, a playlist's tracklist and On-The-Go);
 * everywhere else SELECT still acts on the down-edge, because a delayed tap
 * with no long action behind it would be latency for nothing. The recorded row is what acts: the thumb rocks the
 * wheel during a 450 ms hold, and the row you pressed is the row you meant.
 * The press is dropped if the screen changes, the list is rebuilt
 * (g_list_epoch) or Hold engages under the finger.
 *
 * The cost is that a tap now acts on RELEASE — the original iPod's behaviour,
 * and the same rule Now Playing's SELECT has always used.
 */
/* What a Search hit IS, defined with the rest of that screen below; declared
 * here because the row arbitration is above it. */
static int search_open_hit(fat32_t *fs);

static struct {
    int      pending;
    int      scr;
    int      sub;        /* the screen's own sub-state (rowsel_sub)         */
    int      sel;        /* the row under the thumb at the down-edge        */
    uint32_t epoch;      /* g_list_epoch: the list must not have been rebuilt */
} g_rowsel;

/*
 * Two screens are really two lists behind one screen_t, and a press has to
 * belong to the one it started on: SCR_BROWSER is the album list at depth 0
 * and one album's tracks at depth 1, and SCR_SEARCH is the character ring
 * (where SELECT types and never arbitrates) or the hits.
 */
static int rowsel_sub(void)
{
    switch (scr_cur()) {
    case SCR_BROWSER: return g_dir_depth;
    case SCR_SEARCH:  return g_search.mode;
    default:          return 0;
    }
}

static void rowsel_arm(int sel, keyhold_t *k, uint32_t now_us)
{
    g_rowsel.pending = 1;
    g_rowsel.scr     = (int)scr_cur();
    g_rowsel.sub     = rowsel_sub();
    g_rowsel.sel     = sel;
    g_rowsel.epoch   = g_list_epoch;
    /* Fed here so the press is timed from the EVENT's own tick: the latch can
     * be a pass ahead of clickwheel_buttons(), and a tap short enough to be
     * over before the live sampler sees it would otherwise never be reported
     * at all. */
    (void)keyhold_feed(k, 1, now_us, SEL_HOLD_US);
}

static void rowsel_drop(keyhold_t *k)
{
    g_rowsel.pending = 0;
    keyhold_reset(k);
}

/*
 * The SELECT bodies that used to sit in the event switch, keyed on the
 * RECORDED screen and row. The return is a little bitmask rather than a bool
 * because the caller has two things to set: ROWSEL_DIRTY says the screen
 * needs a repaint, ROWSEL_PLAYING says Now Playing was pushed over a track
 * that has just started (np_first, which skips the first partial paint).
 */
#define ROWSEL_DIRTY   1
#define ROWSEL_PLAYING 2

static int row_select_tap(fat32_t *fs)
{
    int sel = g_rowsel.sel;
    switch (g_rowsel.scr) {
    case SCR_SONGS:
        if (sel < 0 || sel >= g_songview_n) return 0;
        g_song_sel = sel;                  /* the row that acted is the cursor */
        (void)library_play_song(fs, sel);
        hal_volume_set(g_volume);          /* re-apply over codec re-init */
        scr_push(SCR_NOWPLAYING);
        return ROWSEL_DIRTY | ROWSEL_PLAYING;

    case SCR_PLAYLIST:
        if (sel >= g_pl_tracks_n) {        /* the saved slot's Delete row */
            if (g_pl_slot <= 0) return 0;
            if (!otg_confirm_up(&g_pl_confirm)) {
                ui_window_arm(&g_pl_confirm);   /* ask once */
                return ROWSEL_DIRTY;
            }
            g_pl_confirm.armed = 0;
            return otg_delete_slot(fs) ? ROWSEL_DIRTY : 0;
        }
        if (sel < 0 || g_pl_tracks_n == 0) return 0;
        g_plt_sel = sel;
        (void)playlist_play(sel);
        hal_volume_set(g_volume);
        scr_push(SCR_NOWPLAYING);
        return ROWSEL_DIRTY | ROWSEL_PLAYING;

    case SCR_OTG:
        if (sel == OTG_ROW_CLEAR) {
            if (g_otg.n == 0) return 0;
            if (!otg_confirm_up(&g_otg_confirm)) {
                ui_window_arm(&g_otg_confirm);  /* ask once */
                return ROWSEL_DIRTY;
            }
            g_otg_confirm.armed = 0;
            otg_clear(&g_otg);
            otg_bind_all();
            otg_touch();
            g_otg_sel = g_otg_accum = 0;
            otg_flash(-1, "Cleared", 0);
            return ROWSEL_DIRTY;
        }
        if (sel == OTG_ROW_SAVE) {
            if (g_otg.n == 0) return 0;
            return otg_save_to_slot(fs) ? ROWSEL_DIRTY : 0;
        }
        g_otg_sel = sel;
        if (otg_play(sel - OTG_ROW_FIRST) < 0) return 0;
        hal_volume_set(g_volume);
        scr_push(SCR_NOWPLAYING);
        return ROWSEL_DIRTY | ROWSEL_PLAYING;

    case SCR_SEARCH:
        /* Only ever armed in RESULTS mode (the ring's SELECT types a
         * character and must act on the down-edge), so the tap is
         * unambiguously "open this hit" — which is all SEARCH_ACT_OPEN
         * means. */
        if (sel < 0 || sel >= g_search.nhit) return 0;
        g_search.sel = sel;
        return search_open_hit(fs) ? (ROWSEL_DIRTY | ROWSEL_PLAYING)
                                   : ROWSEL_DIRTY;

    case SCR_BROWSER:
        if (g_rowsel.sub == 0) {
            if (sel < 0 || sel >= albumlist_count()) return 0;
            if (albumlist_album_at(sel) < 0) {
                /* "All Songs" for the artist we are filtered to: the whole
                 * discography in title order, so a track you remember but
                 * cannot place to an album is reachable. */
                songview_build(-1, g_artist_filter);
                scr_push(SCR_SONGS);
                return ROWSEL_DIRTY;
            }
            /* Enter the album: load its tracklist + art. g_br_sel stays on the
             * album so backing out lands on it. */
            {
                lib_album_t *al = &g_albums[albumlist_album_at(sel)];
                g_br_sel = sel;
                split_artist_album(al->folder, g_album_artist, g_album_title);
                g_dir_depth = 1;
                browse_load(fs, al->clus);
                detail_load_meta(fs);
                g_det_sel = g_det_accum = 0;
            }
            return ROWSEL_DIRTY;
        }
        if (sel < 0 || sel >= g_browse_n) return 0;
        g_det_sel = sel;
        g_queue_kind = RESUME_KIND_ALBUM;
        g_queue_seed = 0;
        player_play_queue(g_browse, g_browse_n, sel, g_art_clus, g_art_size);
        hal_volume_set(g_volume);
        scr_push(SCR_NOWPLAYING);
        return ROWSEL_DIRTY | ROWSEL_PLAYING;

    default:
        return 0;
    }
}

/* The long press: add to On-The-Go, or remove from it. */
static int row_select_hold(fat32_t *fs)
{
    (void)fs;
    int sel = g_rowsel.sel;
    switch (g_rowsel.scr) {
    case SCR_SONGS:
        if (sel < 0 || sel >= g_songview_n) return 0;
        return otg_add_song(g_songview[sel]);

    case SCR_PLAYLIST:
        if (sel < 0 || sel >= g_pl_tracks_n) return 0;   /* not the Delete row */
        if (g_pl_song[sel] < 0) {
            otg_flash(0, "Not in the library", 0);
            return 1;
        }
        return otg_add_song(g_pl_song[sel]);

    case SCR_OTG:
        if (sel < OTG_ROW_FIRST) return 0;               /* the action rows */
        if (!otg_remove(&g_otg, sel - OTG_ROW_FIRST)) return 0;
        otg_bind_all();
        otg_touch();
        if (g_otg.n == 0) {
            /* The last row went, and the screen is the empty state now: both
             * action rows greyed, the wheel silent. Land on Clear, which is
             * where every other way into that state lands (entry, Clear,
             * Save) — otg_row_count() - 1 would leave the cursor on the
             * greyed Save row with no way to move it off. */
            g_otg_sel = OTG_ROW_CLEAR;
        } else if (g_otg_sel >= otg_row_count()) {
            g_otg_sel = otg_row_count() - 1;             /* the last row went */
        }
        g_otg_accum = 0;
        {
            char tok[16];
            otg_count_token(tok, (int)g_otg.n);
            otg_flash(-1, "Removed", tok);
        }
        return 1;

    case SCR_SEARCH: {
        /* Which hits are addable is ui/search.c's rule (search_hold_rows),
         * so the host can assert it; what a song index MEANS is this file's.
         * The recorded row is what acts, so the selection is restored first. */
        if (sel < 0 || sel >= g_search.nhit) return 0;
        int saved = g_search.sel, addable = 0, si = -1;
        g_search.sel = sel;
        (void)search_hold_rows(&g_search, &addable, &si);
        g_search.sel = saved;
        if (!addable || si < 0 || si >= g_songs_n) return 0;
        return otg_add_song(g_song_sorted[si]);
    }

    case SCR_BROWSER:
        if (g_rowsel.sub == 0) {
            int ai = (sel >= 0 && sel < albumlist_count()) ? albumlist_album_at(sel)
                                                           : -1;
            if (ai < 0) return 0;          /* the All Songs row: nothing to add */
            return otg_add_album(ai);
        }
        /* One track out of the album's tracklist. A Disc header is not a row
         * here — g_browse holds files only — but a file the index never
         * matched has no locator. */
        if (sel < 0 || sel >= g_browse_n) return 0;
        {
            int si = g_browse_song[sel];
            if (si < 0) {
                otg_flash(0, "Not in the library", 0);
                return 1;
            }
            return otg_add_song(si);
        }

    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Presenting: damage-only pushes + partial list repaints
 *
 * lcd_present_fb streams all 38,400 32-bit words to the BCM with IRQs MASKED —
 * the audio DMA is single-shot and re-kicked inside its own ISR, so only the
 * 16-frame I2S FIFO (~363 us) covers a delayed interrupt. Pushing a whole frame
 * to move a selection bar one row is what forced the old fixed 150 ms repaint
 * throttle while playing. So: renderers report what they touched (console.c's
 * damage rect), we present only that, and a plain selection move repaints two
 * rows instead of clearing and redrawing the panel.
 * ------------------------------------------------------------------------- */

/* Measured cost of the last present, used to pace the next one. Seeded to a
 * full-frame-ish value so the first push is paced conservatively. */
/* Declared up with the stats line; seeded to a deliberate over-estimate so
 * the first present is throttled conservatively before it is measured. */
uint32_t g_present_cost_us = 30000u;

/*
 * Scroll diagnostics, reported on a "core: ui" UART line every 5 s in which
 * something was painted (see ui_stats_emit). These exist because "the list
 * lags" has four candidate mechanisms — the render, the present, the cover
 * reads issued under the wheel, and the playing-time throttle — and the only
 * number the log carried was the last present's cost. Every counter is a
 * store or a compare on a path that already reads the timer; none is on the
 * audio path.
 */
static uint32_t g_ui_render_us;        /* CPU time of the last list paint, no present */
static uint32_t g_ui_render_max_us;    /* worst paint in the window                    */
static uint32_t g_ui_present_max_us;   /* worst present in the window                  */
static uint32_t g_ui_full, g_ui_partial;   /* presents in the window, by path        */
static uint32_t g_ui_art_reads;        /* cover loads issued (each a disk read)        */
static uint32_t g_ui_art_max_us;       /* longest single pump: a spin-up lands here    */
static uint32_t g_ui_art_held;         /* passes that refused to wake a parked platter */

static uint32_t present_gap_us(void);  /* defined just below, with the throttle */

/*
 * The drive-power policy's state (kernel/drivepower.h: the idle spin-down,
 * the lock park, the browsing hold, and whether each park was worth it).
 */
static drivepower_t g_drivepower;
/* Park accounting: the timer is read only on a park edge. */
static void disk_park_track(int playing)
{
    int parked = ata_is_parked();
    if (!drivepower_park_edge(&g_drivepower, parked)) return;
    drivepower_park_note(&g_drivepower, parked, playing,
                         mmio_read32(USEC_TIMER_ADDR));
}

static void ui_stats_emit(void)
{
    if (g_ui_full + g_ui_partial == 0 && g_ui_art_reads == 0 && g_ui_art_held == 0) {
        return;                            /* nothing painted: keep the log quiet */
    }
    const player_stats_t *ps = player_stats();
    uart_puts("core: ui full ");        uart_dec((int)g_ui_full);
    uart_puts(" partial ");             uart_dec((int)g_ui_partial);
    uart_puts(" render_us ");           uart_dec((int)g_ui_render_us);
    uart_puts(" max ");                 uart_dec((int)g_ui_render_max_us);
    uart_puts(" present_us ");          uart_dec((int)g_present_cost_us);
    uart_puts(" max ");                 uart_dec((int)g_ui_present_max_us);
    uart_puts(" gap_us ");              uart_dec((int)present_gap_us());
    uart_puts(" art reads ");           uart_dec((int)g_ui_art_reads);
    uart_puts(" max_us ");              uart_dec((int)g_ui_art_max_us);
    uart_puts(" held ");                uart_dec((int)g_ui_art_held);
    uart_puts(" parked ");              uart_dec(ata_is_parked());
    uart_puts(" parks ");               uart_dec((int)drivepower_parks(&g_drivepower, 0));
    uart_puts(" quick ");               uart_dec((int)drivepower_quick_parks(&g_drivepower, 0));
    uart_puts(" parks_play ");          uart_dec((int)drivepower_parks(&g_drivepower, 1));
    uart_puts(" quick ");               uart_dec((int)drivepower_quick_parks(&g_drivepower, 1));
    /* BCM health, cumulative: handshakes that ran out of budget (a post-wake
     * absorb that never retired lands here) and presents refused because the
     * panel was slept. A healthy device reads 0 0 forever. */
    uart_puts(" bcm_timeouts ");        uart_dec((int)lcd_bcm_timeouts());
    uart_puts(" refused ");             uart_dec((int)lcd_presents_refused());
    uart_puts(" decode_us_per_kframe "); uart_dec(ps ? (int)ps->decode_us_per_kframe : -1);
    uart_puts(" ring_low_pct ");        uart_dec((int)player_buf_pct());
    uart_putc('\n');
    g_ui_full = g_ui_partial = 0;
    g_ui_render_max_us = g_ui_present_max_us = 0;
    g_ui_art_reads = g_ui_art_max_us = g_ui_art_held = 0;
}

/* Present whatever has been drawn since the last console_damage_reset(), then
 * clear the damage. A full-screen damage rect (any console_clear) goes out via
 * the full-frame fast path, exactly as before. */
static void ui_present_damage(void)
{
    int x, y, w, h;
    if (console_damage_get(&x, &y, &w, &h)) {
        uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
        int full = (x <= 0 && y <= 0 && w >= LCD_WIDTH && h >= LCD_HEIGHT);
        if (full) {
            lcd_present_fb(console_framebuffer());
        } else {
            lcd_present_rect(console_framebuffer(), x, y, w, h);
        }
        /* Only a FULL present sets the pace. The gap exists so the BCM can
         * retire the last frame to the panel before the next command lands
         * in its short idle budget; a 2.5 ms partial after a 10.8 ms full
         * frame said "10 ms is enough", and the transport band arriving 20 ms
         * after a full Now Playing repaint met a BCM still busy — "idle-wait
         * timed out", re-kicks, a stalled loop and 12 audio underruns (device
         * log, 2026-09-13). */
        if (full) {
            g_present_cost_us = mmio_read32(USEC_TIMER_ADDR) - t0;
        }
    }
    console_damage_reset();
}

/* How long to wait between repaints WHILE PLAYING. Self-tuning: space repaints
 * at ~4x what the last present actually cost. A cheap partial present paces
 * fast (a responsive wheel), a full-frame push still backs off to about the
 * old fixed 150 ms — but only when it really is a full frame.
 *
 * The 4x was chosen when the pixel push ran with IRQs MASKED end to end, to
 * keep that masked time under ~1/4 of the loop. lcd.c now releases the I-bit
 * between panel rows, so the DMA re-kick deadline no longer bears on this; what
 * the gap still buys is CPU for the decoder. player_pump decodes exactly one
 * 1024-frame step per pass (~23 ms of audio), so a pass that also renders and
 * presents yields less audio than it consumes, and the gap is what keeps such
 * passes the minority. Whether 4x is the right share is a question for the
 * "core: ui" line (render_us, present_us, decode_us_per_kframe, ring_low_pct):
 * on the album list every detent scrolls the window (ui_scroll_window anchors
 * the selection a third of the way down), so every scroll present is a FULL
 * frame and this multiplier is the frame rate. Do not lower it on a guess. */
static uint32_t present_gap_us(void)
{
    uint32_t gap = g_present_cost_us * 4u;
    if (gap < 20000u)  gap = 20000u;      /* <=50 fps: no point going faster    */
    if (gap > 150000u) gap = 150000u;     /* the old worst-case throttle        */
    return gap;
}

/* A list screen described generically, so one routine can repaint the rows that
 * changed without knowing which screen it is. `row` draws list row r showing
 * item `idx` (it reads that screen's own selection to decide highlighting). */
typedef struct {
    const char *title;                 /* header title (for the overlap check) */
    int         count, visible, rh, y0, sel;
    void      (*row)(int r, int idx);
    char        right[16];             /* header "n / m" value ("" = none)     */
} list_view_t;

/* Describe the screen on top of the stack, or return 0 if it isn't a list that
 * can be partially repainted (Settings/Charging/Now Playing render elsewhere,
 * empty lists draw a placeholder instead of rows). */
static int list_view_current(list_view_t *v)
{
    v->right[0] = '\0';
    v->rh       = ROW_H;
    v->y0       = LIST_Y0;
    v->visible  = LIST_ROWS;
    switch (scr_cur()) {
    case SCR_MENU:
        g_menu_items = g_main_menu;
        g_menu_sel   = g_main_sel;
        v->title = "Core";
        v->count = main_menu_count();
        v->sel   = g_main_sel;
        v->row   = menu_row_draw;
        break;
    case SCR_MUSIC:
        g_menu_items = g_music_menu;
        g_menu_sel   = g_music_sel;
        v->title = "Music";
        v->count = MU_COUNT;
        v->sel   = g_music_sel;
        v->row   = menu_row_draw;
        break;
    case SCR_ARTISTS:
        v->title = "Artists";
        v->count = g_artists_n;
        v->sel   = g_artist_sel;
        v->row   = artists_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_SONGS:
        v->title   = g_songview_artist[0] ? g_songview_artist : "Songs";
        v->count   = g_songview_n;
        v->sel     = g_song_sel;
        v->row     = songs_row_draw;
        v->rh      = ROW_H2;
        v->visible = LIST_ROWS2;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_GENRES:
        v->title = "Genres";
        v->count = g_genres_n;
        v->sel   = g_genre_sel;
        v->row   = genres_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_QUEUE:
        v->title = "Now Playing";
        v->count = player_queue_len();
        v->sel   = g_queue_sel;
        v->row   = queue_row_draw;
        fmt_count(v->right, player_queue_current() + 1, v->count);
        break;
    case SCR_PLAYLISTS:
        /* The pinned row is always there, but a folder that is empty or would
         * not read draws its sentence UNDER it — a placeholder, not a row —
         * so those two states repaint whole. */
        if (g_playlists_err || g_playlists_n == 0) return 0;
        v->title = "Playlists";
        v->count = playlists_row_count();   /* the pinned On-The-Go row is row 0 */
        v->sel   = g_pl_sel;
        v->row   = playlists_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_PLAYLIST:
        /* Same: with no tracks the screen is a reason plus (on a saved slot)
         * the Delete row, laid out by playlist_render and not by this table. */
        if (g_pl_tracks_n == 0) return 0;
        v->title   = g_pl_open >= 0 ? g_playlists[g_pl_open].name : "Playlist";
        v->count   = playlist_row_count();  /* + the saved slot's Delete row */
        v->sel     = g_plt_sel;
        v->row     = playlist_row_draw;
        v->rh      = ROW_H2;
        v->visible = LIST_ROWS2;
        if (v->sel < g_pl_tracks_n) {
            fmt_count(v->right, v->sel + 1, g_pl_tracks_n);
        }
        break;
    case SCR_OTG:
        /* An empty list draws only its two sentences — the greyed action rows
         * are not drawn at all, because neither of them does anything — so it
         * is not a partially-repaintable list. */
        if (g_otg.n == 0) return 0;
        v->title   = "On-The-Go";
        v->count   = otg_row_count();
        v->sel     = g_otg_sel;
        v->row     = otg_row_draw;
        v->rh      = ROW_H2;
        v->visible = LIST_ROWS2;
        otg_header_right(v->right, v->sel);
        break;
    case SCR_BROWSER:
        if (g_dir_depth == 0) {
            v->title   = g_artist_filter[0] ? g_artist_filter : "Albums";
            v->count   = albumlist_count();
            v->sel     = g_br_sel;
            v->row     = albumlist_row_draw;
            v->rh      = ROW_H2;
            v->visible = LIST_ROWS2;
            fmt_count(v->right, v->sel + 1, v->count);
        } else {
            /* The tracklist scrolls over the DISPLAY view (tracks interleaved
             * with "Disc N" headers), so the row space is view indices. */
            v->title   = "Albums";
            v->count   = g_det_view_n;
            v->sel     = detail_sel_view(g_det_sel);
            v->row     = detail_row_draw;
            v->y0      = DET_LIST_Y0;
            v->visible = DET_ROWS;
            fmt_count(v->right, g_det_sel + 1, g_browse_n > 0 ? g_browse_n : 1);
        }
        break;
    default:
        return 0;
    }
    return v->count > 0;
}

/* What the last paint showed, so the next one can tell a plain selection move
 * from a change that needs the whole panel. `chrome` fingerprints the shared
 * furniture (status strip name, battery, lock, playing row) — if any of it
 * moved, a two-row repaint would leave the screen stale. */
static struct {
    int      valid, scr, depth, sel, top, count, right_w;
    uint32_t chrome, epoch;
    uint32_t rows;      /* bit r: row r's content changed under a still view
                         * (a cover chip arrived) — repaint it, nothing else */
} g_lp;

static uint32_t chrome_key(void)
{
    uint32_t k = player_active() ? 1u : 0u;
    k = k * 31u + (player_paused() ? 1u : 0u);
    k = k * 31u + (uint32_t)(player_queue_current() + 1);
    k = k * 31u + (uint32_t)(g_locked ? 1 : 0);
    k = k * 31u + (uint32_t)(g_bat_pct + 1);
    /* The strip's SLEEP token, for the same reason the battery percentage is
     * here: a partial two-row repaint under a changed minute would leave a
     * stale token on the panel. */
    k = k * 31u + (uint32_t)sleeptimer_remaining_min(&g_sleep);
    return k;
}

/* Record what we just painted (called after every paint of a list screen). */
static void list_paint_note(void)
{
    list_view_t v;
    if (!list_view_current(&v)) {
        g_lp.valid = 0;
        return;
    }
    g_lp.valid   = 1;
    g_lp.scr     = (int)scr_cur();
    g_lp.depth   = g_dir_depth;
    g_lp.sel     = v.sel;
    g_lp.count   = v.count;
    g_lp.top     = ui_scroll_window(v.sel, v.count, v.visible);
    g_lp.right_w = v.right[0] ? text_width(v.right, FONT_SMALL) : 0;
    g_lp.chrome  = chrome_key();
    g_lp.epoch   = g_list_epoch;
    g_lp.rows    = 0;                     /* a full paint drew every row       */
}

/* Repaint the current screen by redrawing ONLY what changed under a still
 * view: the two rows a selection move touched (plus the header's "n / m" value
 * and the scrollbar), and any row whose cover chip arrived since the last
 * paint (g_lp.rows). Returns 0 when that isn't provably sufficient (different
 * screen, scrolled window, changed contents or chrome) — the caller then does
 * the full render.
 *
 * The chip case used to fall through to the full render: with the selection
 * unchanged there was "nothing" for the partial path to do, so each cover that
 * landed on the album list cleared and re-pushed the whole panel — six times
 * over for a fresh window, each one re-arming the playing-time throttle at the
 * full-frame cost, in the middle of the scroll the covers were loading for. */
static int list_repaint_partial(void)
{
    list_view_t v;
    if (!g_lp.valid || !list_view_current(&v)) return 0;
    if (g_lp.scr != (int)scr_cur() || g_lp.depth != g_dir_depth) return 0;
    if (g_lp.epoch != g_list_epoch)                              return 0;
    if (g_lp.count != v.count || g_lp.chrome != chrome_key())    return 0;
    int moved = (v.sel != g_lp.sel);
    if (!moved && g_lp.rows == 0)                                return 0;
    int top = ui_scroll_window(v.sel, v.count, v.visible);
    if (top != g_lp.top) return 0;        /* window scrolled: every row moved   */

    uint32_t rows = g_lp.rows;
    if (moved) {
        int r_old = g_lp.sel - top, r_new = v.sel - top;
        if (r_old < 0 || r_old >= v.visible || r_new < 0 || r_new >= v.visible) {
            return 0;                      /* off-window selection: play it safe */
        }

        /* Header value: clear only as wide as the old/new text. If the title
         * reaches into that box, the clear would eat it — fall back to a full
         * paint. */
        int rw = v.right[0] ? text_width(v.right, FONT_SMALL) : 0;
        int cw = ((rw > g_lp.right_w) ? rw : g_lp.right_w) + 4;
        if (cw > 4) {
            int cx = LCD_WIDTH - 12 - cw;
            if (12 + 20 + text_width(v.title, FONT_HEADER) > cx) return 0;
            console_fill_rect(cx, HDR_BASE - 12, cw, 16, LINEN_SURFACE);
            if (rw) {
                ui_text(LCD_WIDTH - 12 - rw, HDR_BASE - 1, v.right, FONT_SMALL,
                        LINEN_MUTED2);
            }
        }
        rows |= (1u << r_old) | (1u << r_new);
    }

    /* The rows: clear each band (the new content may be narrower) + redraw.
     * The clear stops short of the scrollbar column (ui_list_row_clear). A
     * full-width clear here, with the bar only redrawn on a MOVE below, is how
     * the album list's scrollbar vanished under a fast scroll: the covers
     * landing on the settled window repainted every row band edge to edge and
     * took the bar with them, slice by slice, until the next full paint. */
    for (int r = 0; r < v.visible; r++) {
        if (!(rows & (1u << r))) continue;
        ui_list_row_clear(v.y0, r, v.rh);
        int idx = top + r;
        if (idx < v.count) v.row(r, idx);
    }
    /* Unconditional, not just on `moved`: every partial present that touches a
     * row band carries the bar with it, so no row repaint can leave the panel
     * without one. Two small fills — nothing to save by skipping them. (A
     * no-op when everything fits, exactly as in the full render.) */
    ui_scrollbar(v.y0, top, v.visible, v.count);
    g_lp.rows = 0;
    return 1;
}

/* Cover loads under a moving wheel (the album-list pump in run_ui; see the
 * block comment there). A detent within SETTLE means the list is scrolling:
 * one disk read per pass, not six. A parked platter is only woken for covers
 * after QUIET of wheel silence — long enough that a deliberate detent-by-
 * detent scroll (~300 ms apart) never trips a spin-up mid-gesture. */
#define CHIP_WHEEL_SETTLE_US   150000u
#define CHIP_SPINUP_QUIET_US   500000u
/* Rows past the window the cover pump also loads, once every visible row is
 * resident: a screen ahead in the direction of travel and a screen behind.
 * Without them every newly revealed row waited for its read and then
 * re-presented its band when the cover landed: two paints per detent. */
#define CHIP_AHEAD_ROWS        LIST_ROWS2

/* ---------------------------------------------------------------------------
 * The A-Z locator: which letter a row is under, on whichever list is up
 *
 * This used to be SONGS ONLY, for one reason: the locator's only tool was a
 * linear walk of the rows, which is affordable exactly once. ui/letterindex.c
 * turns the per-row answer into a run index built once per list, so every
 * long alphabetised list can have the plate and the letter stepping —
 * Songs, a genre's songs, an artist's All Songs, Artists, Albums (all and one
 * artist's), Playlists and Genres.
 *
 * What differs per screen is the KEY the list is sorted BY, and that is the
 * only thing main.c has to answer: Artists sort past a leading "The " (so
 * "The Kid LAROI" is under K), Albums by the album half of the folder name,
 * Songs/Playlists/Genres by the name itself. Returning 0 for a row (or for a
 * whole screen) is what keeps the queue, the tracklists, the menus and
 * Settings out of it — and it is the same 0 wheel_move()'s guard reads to
 * fall through to row scrolling.
 * ------------------------------------------------------------------------- */

/* The initial of the key row `row` is SORTED BY, or 0 for a row that is not
 * part of the order (the album list's synthetic "All Songs" row) and for
 * every screen that has no alphabetical order to locate within. */
static char screen_initial_raw(int row)
{
    switch (scr_cur()) {
    case SCR_SONGS:
        if (row >= 0 && row < g_songview_n) {
            return initial_of(g_songs[g_songview[row]].title);
        }
        return 0;
    case SCR_ARTISTS:
        if (row >= 0 && row < g_artists_n) {
            return initial_of(artist_key(g_artists[row].name));
        }
        return 0;
    case SCR_GENRES:
        if (row >= 0 && row < g_genres_n) {
            return initial_of(g_genres[row]);
        }
        return 0;
    case SCR_PLAYLISTS:
        if (row >= 0 && row < g_playlists_n) {
            return initial_of(g_playlists[row].name);
        }
        return 0;
    case SCR_BROWSER: {
        /* Depth 0 is the album list; depth 1 is one album's tracklist, which
         * is in track order and has no letters. albumlist_album_at answers -1
         * for the "All Songs" row and for a row off the end. */
        if (g_dir_depth != 0) return 0;
        int a = albumlist_album_at(row);
        return (a >= 0) ? initial_of(g_album_key[a]) : 0;
    }
    default:
        return 0;
    }
}

/* The index, and the list it was built for. g_list_epoch already bumps on
 * every content rebuild and every push/pop, so this is one compare per call
 * in the common case and a single walk when the list underneath changes. */
static letteridx_t g_letters;
static struct { int scr, depth, count; uint32_t epoch; } g_letters_for = {
    -1, -1, -1, 0
};

/* Bring the index up to date with whatever list is on screen, and hand back
 * that list's view (count 0 on a screen that is not a list). */
static void letters_ensure(list_view_t *v)
{
    v->count = 0;
    v->sel   = 0;
    (void)list_view_current(v);
    int scr = (int)scr_cur();
    if (g_letters_for.scr   == scr        && g_letters_for.depth == g_dir_depth &&
        g_letters_for.count == v->count   && g_letters_for.epoch == g_list_epoch) {
        return;
    }
    g_letters_for.scr   = scr;
    g_letters_for.depth = g_dir_depth;
    g_letters_for.count = v->count;
    g_letters_for.epoch = g_list_epoch;
    (void)letteridx_build(&g_letters, v->count, screen_initial_raw);
}

/* The ui/wheel.h seam: the locator letter for row `idx`, 0 where there is
 * none. Also the guard that decides whether this screen letter-steps at all. */
static char list_initial_at(int idx)
{
    list_view_t v;
    letters_ensure(&v);
    return letteridx_letter_at(&g_letters, idx);
}

/* The letter the plate shows: the selected row's. */
static char list_sel_initial(void)
{
    list_view_t v;
    letters_ensure(&v);
    return letteridx_letter_at(&g_letters, v.sel);
}

/* The ui/wheel.h letter-step seam: where one letter detent lands. `count` is
 * the wheel's view of the list and the index's own is the same number (both
 * come from list_view_current), so the index carries it. */
static int list_letter_step_idx(int sel, int count, int dir)
{
    list_view_t v;
    (void)count;
    letters_ensure(&v);
    return letteridx_step(&g_letters, sel, dir);
}

/* The wheel's clock (the ui/wheel.h seam): the free-running USEC_TIMER. */
static uint32_t wheel_clock(void)
{
    return mmio_read32(USEC_TIMER_ADDR);
}

/* The letter itself, on a centred plate over the flying list. Its presence is
 * exactly the signal that the wheel is stepping LETTERS rather than rows —
 * there is deliberately no state where the plate is up and the wheel is still
 * grinding through individual songs. */
static void az_overlay_render(char ch)
{
    const int PW = 66, PH = 66;
    int px = (LCD_WIDTH - PW) / 2, py = (LCD_HEIGHT - PH) / 2;
    ui_round_rect(px - 1, py - 1, PW + 2, PH + 2, 13, LINEN_BORDER);
    ui_round_rect(px, py, PW, PH, 12, LINEN_PLATE);
    char s[2] = { ch, '\0' };
    int w = text_width(s, FONT_TITLE);
    ui_text(px + (PW - w) / 2, py + PH / 2 + 8, s, FONT_TITLE, LINEN_INK);

#if AZ_SHOW_TPS
    {
        char n[12];
        u32_to_dec(n, wheel_tps());
        int nw = text_width(n, FONT_SMALL);
        ui_text(px + (PW - nw) / 2, py + PH - 4, n, FONT_SMALL, LINEN_MUTED_D);
    }
#endif
}

/* Busy-wait `us` microseconds (PWM already off) — for the silent gap in the
 * multi-burst clicker profiles. Short enough not to underrun the decode buffer. */
static void ui_spin_us(uint32_t us)
{
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us) { /* spin */ }
}

/* Piezo navigation click. g_settings.clicker selects the sound profile (0 = Off;
 * 1..7 = Tick / Click / Pop / Blip / Tock / Double / Chirp). Single-tone
 * profiles are one (Hz, µs) burst; Double is two bursts with a gap; Chirp is a
 * rising three-tone sweep. Order matches settings.c CLICK_L. */
static void ui_click(void)
{
    switch (g_settings.clicker) {
        case 1: piezo_click_ex(3000, 3000); break;   /* Tick  — crisp mid tick   */
        case 2: piezo_click_ex(4500, 2000); break;   /* Click — higher, shorter  */
        case 3: piezo_click_ex(1800, 5000); break;   /* Pop   — lower, fuller    */
        case 4: piezo_click_ex(6000, 1500); break;   /* Blip  — high, ultra-short*/
        case 5: piezo_click_ex(1000, 4000); break;   /* Tock  — low, woody       */
        case 6:                                       /* Double — two quick taps  */
            piezo_click_ex(4200, 1500);
            ui_spin_us(22000);
            piezo_click_ex(4200, 1500);
            break;
        case 7:                                       /* Chirp — rising sweep     */
            piezo_click_ex(3000, 1100);
            piezo_click_ex(4000, 1100);
            piezo_click_ex(5200, 1200);
            break;
        default: break;                               /* 0 = Off                  */
    }
}

/* ---------------------------------------------------------------------------
 * Resume playback position across a power cycle.
 *
 * WHAT IS STORED, AND WHY IT IS A NAME
 *
 * settings_t carries three fields (resume_hash / resume_secs / resume_total)
 * that ride along in the CORECFG.DAT record. The locator is the folded
 * name_hash() of the track's ext-trimmed FILENAME as the queue displays it
 * (the on-disk stem, which is also lib_song_t.stem_hash once a record has
 * bound to its file) — not a queue index, not a song index, not a cluster. Every one of those is a statement about the
 * library as it happened to be laid out when we saved: re-import the music,
 * rebuild CORELIB.IDX, add one album, and index 412 is a different song while
 * cluster 918233 may be somebody else's file. The filename is the only handle
 * that means the same thing on the other side of a rebuild, which is exactly
 * why the library already binds records to files by this same hash.
 *
 * A name is not unique, though — "01 Intro.flac" lives in half the albums on a
 * real library — so resume_total (the decoder's track length) is kept as a
 * cross-check and the restore declines an ambiguous match outright. Resuming
 * nothing is a shrug; resuming the WRONG track is a bug.
 *
 * WHAT ELSE IS STORED: THE QUEUE
 *
 * A track alone is not where the user was — it was the 412th of Songs, or
 * halfway through a Shuffle Songs draw, and coming back in its ALBUM
 * instead (which is all the record used to allow) is the "the playlist
 * switches to the album" bug. Every queue this firmware builds is a
 * function of the library plus a few words, so the record carries those
 * words (settings.h, RESUME_KIND_*): the kind of queue; for Songs / an
 * artist / a genre nothing more, because songview_build() over the resumed
 * song's own artist or genre is that list again; for Shuffle Songs the LCG
 * seed the library order was dealt from; and the player's own shuffle deal
 * as its (seed, keep) pair, so Next after the power cut is still the track
 * that was going to come next. Suspend keeps all of this in RAM; this is
 * for the cold boot — PMU standby, the battery cutting out, a battery pull.
 * The album stays the fallback for a record from before the context
 * existed and for any rebuild that does not land on the track.
 *
 * WHEN IT IS CAPTURED (the write budget)
 *
 * Never per second, and never per pass. The position is snapshotted only at
 * moments where it actually changed in a way worth a kilobyte of disk:
 *
 *   - the track changed (player_open_seq bumped) — the important half is
 *     WHICH track, and that is the only moment it moves;
 *   - pause/unpause — the "I'm putting this down" moment;
 *   - playback started or stopped;
 *   - otherwise at most once per RESUME_SAMPLE_US while a track plays, so a
 *     battery pull mid-album loses minutes, not the album;
 *   - forced on the way into suspend / PMU standby, before the drive parks.
 *
 * Each of those calls settings_touch(), so it inherits the existing debounce
 * AND the "don't spin the platters up for 1 KB" guard in settings_commit() —
 * a capture made while the drive is parked rides out on the next time the
 * platters turn (an anti-skip refill, say) rather than costing its own
 * spin-up. Continuous playback therefore costs roughly one
 * write per track (~20/hour); an idle or paused device costs none.
 * ------------------------------------------------------------------------- */

/* Longest a single track may play before we re-stamp the position. 5 minutes:
 * a full-album track rarely outlives it, so in practice the track-change edge
 * is what fires and this is only the backstop for long mixes and podcasts. */
#define RESUME_SAMPLE_US   300000000u          /* 300 s */

/* Below this, don't bother restoring a position — the user is at the top of
 * the track for any purpose they'd notice, and it saves a seek. */
#define RESUME_MIN_SECS    10u

/* How far the library's indexed duration may differ from the decoder's before
 * we stop believing the two describe the same file. Both are truncated to
 * whole seconds from different sources, so they disagree by a second on
 * perfectly good matches. */
#define RESUME_DUR_SLOP    2u

static uint32_t g_resume_open_seq;      /* player_open_seq() when last captured */
static uint32_t g_resume_last_us;       /* when we last considered capturing     */
static uint32_t g_resume_end_seq;       /* last seen player_end_seq()            */
static int      g_resume_was_paused;    /* pause state when last captured       */

/*
 * Snapshot the playing track + position into g_settings (debounced onto disk
 * by settings_commit). Cheap and idempotent: re-capturing the same track at
 * the same second does not mark anything dirty, so the callers below can be
 * liberal about when they call it.
 */
void resume_capture(void)
{
    if (!g_settings.resume_on_startup) {
        /* Switched off: don't merely stop recording — drop what is already
         * stored. Otherwise turning it back on next month resumes whatever the
         * user was listening to before they turned it off, which reads as the
         * device remembering something it was told to forget. */
        if (resume_ctx_clear(&g_settings)) {
            settings_touch();
        }
        return;
    }
    if (!player_active()) {
        return;                        /* nothing loaded — keep the last one */
    }
    const char *nm = player_track_name();
    if (nm == 0 || nm[0] == '\0') {
        return;
    }

    /* The locator, plus what the track is playing IN: the queue kind and seed
     * the builder recorded, where in the queue it sits, and the player's
     * shuffle deal — enough for resume_restore to build the same queue and
     * the same order, so Next after a power cut is the track that was going
     * to come next. */
    resume_ctx_t c;
    c.hash       = name_hash(nm);
    c.secs       = player_elapsed_s();
    c.total      = player_total_s();
    c.kind       = g_queue_kind;
    c.qidx       = player_queue_current();
    c.seed       = g_queue_seed;
    c.order_seed = player_order_seed();
    c.order_keep = player_order_keep();
    c.ctx_hash   = (g_queue_kind == RESUME_KIND_PLAYLIST) ? g_queue_ctx_hash : 0;
    if (resume_ctx_store(&g_settings, &c)) {
        settings_touch();              /* something moved — schedule a write */
    }
}

/*
 * Find the library song whose on-disk stem folds to `hash`. Returns a
 * g_songs index, or -1 when there is no safe answer.
 *
 * "Safe" is the whole point. A name match that the duration also confirms is
 * taken immediately. A name match with no confirmation is taken only when the
 * name is UNIQUE across the library — otherwise we would be picking one of
 * several "01 Intro.flac" at random, and a coin-flip is not a resume.
 *
 * The name compared is stem_hash — the hash of the name the file HAS, taken
 * from its directory entry when the record bound, which is the same string
 * the queue shows and resume_capture hashed. It used to be name_hash(file)
 * over the index's copy of the filename, and for a filename past 63 bytes
 * that copy is a truncated string that hashes like nothing on the disk: the
 * track played, the position was saved, and the restore never found it. A
 * record that never bound keeps that provisional hash — exact for a short
 * name, matching nothing for a long one — so it can still count as a
 * same-named twin below, but can never be the song that is opened.
 *
 * One linear pass over g_songs at boot: ~6000 integer compares, well under a
 * millisecond, against a library load that already took seconds.
 */
static int resume_find_song(uint32_t hash, uint32_t total_s)
{
    int best = -1, n_named = 0;

    for (int i = 0; i < g_songs_n; i++) {
        if (hash == 0 || g_songs[i].stem_hash != hash) {
            continue;
        }
        n_named++;
        if (!g_songs[i].file_clus) {
            continue;                  /* indexed but not on the disk any more */
        }
        uint32_t d = g_songs[i].duration_s;
        if (d != 0 && total_s != 0 &&
            d + RESUME_DUR_SLOP >= total_s && total_s + RESUME_DUR_SLOP >= d) {
            return i;                  /* name AND length agree — this is it */
        }
        if (best < 0) {
            best = i;
        }
    }
    return (n_named == 1) ? best : -1;
}

/*
 * Did a rebuilt queue land on the saved track? The builders skip forward
 * over a track they cannot open — right for a user pressing SELECT, wrong
 * for a silent restore: being handed a different song than the one you left
 * is precisely the failure this whole path is written to avoid. The guard is
 * the same locator the capture wrote: the current entry's name must hash to
 * resume_hash. (Run under the mute; the caller pauses.)
 */
static int resume_landed(void)
{
    if (!player_active()) {
        return 0;
    }
    const char *nm = player_queue_name(player_queue_current());
    return nm[0] != '\0' && name_hash(nm) == g_settings.resume_hash;
}

/*
 * The track's ALBUM — the folder it lives in, as the browser would list it.
 * The one context that needs nothing but the song, so it is the fallback
 * for every other kind.
 *
 * browse_collect() only lists FILES at depth 1 (depth 0 is the album list),
 * so borrow the depth for the read and hand it straight back. The browser's
 * copy of the listing is the caller's to clear.
 */
static int resume_open_album(fat32_t *fs, int si)
{
    int saved_depth = g_dir_depth;
    g_dir_depth = 1;
    uint32_t rt0 = boot_ms_now();
    browse_load(fs, g_songs[si].dir_clus);
    g_boot_res_dir_ms = boot_ms_now() - rt0;
    g_dir_depth = saved_depth;

    /* The row is the file the song bound to: same cluster. Not the name — the
     * row's name and the song's are only the same string BECAUSE the song
     * bound, and the cluster is the binding itself. */
    int idx = -1;
    for (int i = 0; i < g_browse_n; i++) {
        if (!g_browse[i].is_dir && g_browse[i].clus == g_songs[si].file_clus) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return 0;                      /* the folder no longer holds the file */
    }
    g_queue_kind = RESUME_KIND_ALBUM;
    g_queue_seed = 0;
    player_play_queue(g_browse, g_browse_n, idx, g_art_clus, g_art_size);
    return player_queue_current() == idx && resume_landed();
}

/*
 * Songs / an artist's songs / a genre's songs. songview_build() matches by
 * the song's own artist (folded through artist_key, exactly as the Artists
 * list merged them) or genre, so building it again from the resumed song's
 * fields is the list the user picked from — no extra key had to be saved.
 * The saved queue index is only a hint: it is where the song sat last time,
 * and after a library change it is checked, not trusted.
 */
static int resume_open_view(fat32_t *fs, int si, int kind)
{
    const lib_song_t *s = &g_songs[si];
    switch (kind) {
    case RESUME_KIND_SONGS:  songview_build(-1, 0);             break;
    case RESUME_KIND_ARTIST: songview_build(-1, s->artist);     break;
    case RESUME_KIND_GENRE:
        if (s->genre < 0) return 0;    /* untagged: it was never in a genre */
        songview_build(s->genre, 0);
        break;
    default:
        return 0;
    }
    int vi = -1, hint = g_settings.resume_qidx;
    if (hint < g_songview_n && g_songview[hint] == si) {
        vi = hint;
    }
    for (int i = 0; vi < 0 && i < g_songview_n; i++) {
        if (g_songview[i] == si) vi = i;
    }
    if (vi < 0) {
        return 0;                      /* the song is not in that list now */
    }
    return library_play_song(fs, vi) >= 0 && resume_landed();
}

/* Shuffle Songs: the library dealt again from the saved seed. The draw is
 * a pure function of (seed, library size), so over an unchanged library it
 * is the same queue in the same order; over a changed one it is a shuffle
 * that still contains the track, which resume_landed() confirms. */
static int resume_open_shuffle(fat32_t *fs, int si)
{
    if (g_settings.resume_seed == 0) {
        return 0;
    }
    return shuffle_songs_build(fs, g_settings.resume_seed, si) >= 0 &&
           resume_landed();
}

/*
 * A playlist: the .m3u8 whose ext-trimmed filename hashes to the saved
 * context, re-read and re-resolved exactly as opening it from the
 * Playlists screen does, then played from the row that holds the song.
 * The row is the song's file — matched by cluster, the binding itself —
 * with the saved queue index as the first guess, so a playlist that lists
 * the track twice comes back on the copy that was playing. A renamed or
 * deleted playlist, or one that no longer lists the track, declines and
 * the caller falls back to the album. The read is counted under the
 * "dir" boot phase (it is directory walks: one per entry).
 */
static int resume_open_playlist(fat32_t *fs, int si)
{
    uint32_t want = g_settings.resume_ctx_hash;
    if (want == 0) {
        return 0;
    }
    playlists_load(fs);
    int pi = -1;
    for (int i = 0; i < g_playlists_n; i++) {
        if (g_playlists[i].hash == want) {
            pi = i;
            break;
        }
    }
    if (pi < 0) {
        return 0;                      /* renamed or deleted */
    }
    g_pl_sel = pi + 1;                 /* Playlists opens on it later (row 0
                                        * is the pinned On-The-Go row) */
    uint32_t rt0 = boot_ms_now();
    playlist_open(fs, pi);
    g_boot_res_dir_ms = boot_ms_now() - rt0;

    uint32_t fc  = g_songs[si].file_clus;
    int idx = -1, hint = g_settings.resume_qidx;
    if (hint < g_pl_tracks_n && g_pl_tracks[hint].clus == fc) {
        idx = hint;
    }
    for (int i = 0; idx < 0 && i < g_pl_tracks_n; i++) {
        if (g_pl_tracks[i].clus == fc) idx = i;
    }
    if (idx < 0) {
        return 0;                      /* the playlist no longer lists it */
    }
    g_plt_sel = idx;
    return playlist_play(idx) == idx && resume_landed();
}

/*
 * The On-The-Go live list. Nothing has to be found: COREOTG.DAT was loaded and
 * bound before this runs, so the queue is already describable. The only
 * question is WHICH entry the saved track is, and the saved queue index is
 * only a hint — it is the index into the QUEUE, which skips unresolved
 * entries, so it equals the entry index only when nothing before it was
 * missing. Check the hint, then scan.
 */
static int resume_open_otg(fat32_t *fs, int si)
{
    (void)fs;
    if (g_otg.n == 0) {
        return 0;                      /* nothing was restored, or it was cleared */
    }
    int idx = -1, hint = g_settings.resume_qidx;
    if (hint < (int)g_otg.n && g_otg_song[hint] == (int16_t)si) {
        idx = hint;
    }
    for (int i = 0; idx < 0 && i < (int)g_otg.n; i++) {
        if (g_otg_song[i] == (int16_t)si) idx = i;
    }
    if (idx < 0) {
        return 0;                      /* the list no longer holds it */
    }
    g_otg_sel   = OTG_ROW_FIRST + idx;  /* On-The-Go opens on it later */
    g_otg_accum = 0;
    return otg_play(idx) >= 0 && resume_landed();
}

/*
 * Re-open the saved track at the saved position, PAUSED, at boot — in the
 * queue it was playing in, with the same shuffle order, so Next after a
 * power cut is the track that was going to come next.
 *
 * The queue is rebuilt from the saved kind: Songs, an artist, a genre, a
 * Shuffle Songs draw or a playlist, each from the resumed song's own fields
 * plus the seed (or the playlist's name hash) the record carries. When the
 * kind is unknown (a record from before the
 * context existed), the album, or a rebuild that does not land on the track
 * (the library changed under it), the track's ALBUM is built instead — the
 * one context that needs nothing but the song.
 *
 * Three things this deliberately does NOT do:
 *
 *   - it does not play. Coming back on and having music start on its own is
 *     hostile, so the transport is left paused with the position already set;
 *     the user presses PLAY. The codec is muted across every open because
 *     player_open_current() unconditionally starts the DAC off a primed ring —
 *     without the mute there is a click between the open and the pause;
 *   - it does not guess. Any failure at every step — setting off, no locator,
 *     no unambiguous song, neither the saved queue nor the album holding the
 *     file, an open falling through to a different track — leaves the device
 *     exactly as if nothing had been saved, codec powered down;
 *   - it does not guess a position it cannot reach cheaply. Both formats seek
 *     in a couple of reads now — FLAC through its SEEKTABLE or a binary search,
 *     MP3 through the Xing TOC — so both are cued to the saved second. MP3 was
 *     cued at 0:00 for as long as dr_mp3's only seek was a scan from the start,
 *     which for a podcast resumed at 50 minutes froze the boot path.
 */
static void resume_restore(fat32_t *fs)
{
    if (!g_settings.resume_on_startup || g_settings.resume_hash == 0) {
        return;
    }
    int si = resume_find_song(g_settings.resume_hash, g_settings.resume_total);
    if (si < 0) {
        return;                        /* deleted, renamed, or ambiguous */
    }

    /* Mute BEFORE any open: audio_bringup() re-latches whatever gain the HAL
     * currently holds, so a 0 here means the DAC comes up silent and the
     * pause lands before a single audible sample. Restored at the bottom. */
    hal_volume_set(0);
    uint32_t ot0  = boot_ms_now();
    uint32_t seq0 = player_open_seq();   /* bumps iff a track was opened */
    int kind = g_settings.resume_kind;
    int ok   = 0;
    switch (kind) {
    case RESUME_KIND_SONGS:
    case RESUME_KIND_ARTIST:
    case RESUME_KIND_GENRE:   ok = resume_open_view(fs, si, kind); break;
    case RESUME_KIND_SHUFFLE: ok = resume_open_shuffle(fs, si);    break;
    case RESUME_KIND_PLAYLIST: ok = resume_open_playlist(fs, si);  break;
    case RESUME_KIND_OTG:     ok = resume_open_otg(fs, si);        break;
    default:                  break;   /* album or none: the fallback below */
    }
    if (!ok) {
        ok = resume_open_album(fs, si);
    }
    if (ok) {
        player_pause();
    }
    g_boot_res_open_ms = boot_ms_now() - ot0;

    /* The album path borrowed the browser's listing; hand it back empty on
     * every exit, success included. The player copied what it needed, and
     * a listing left behind at depth 0 is one the album list never shows
     * but every depth-1 accessor would still read. */
    g_browse_n = 0;
    g_cur_dir  = 0;
    g_art_clus = g_art_size = 0;

    if (!ok) {
        /* A rebuild that opened the wrong track, or none: stop, and power
         * the codec down — an open brought the WM8758 up, and a bare
         * player_stop() would leave it drawing until the next play. Only
         * if one did: with nothing ever opened the codec was never up. */
        player_stop();
        if (player_open_seq() != seq0) {
            hal_audio_close();
        }
        hal_volume_set(g_volume);
        return;
    }

    /* The shuffle order. The build above dealt a fresh one (a boot's worth
     * of timer entropy); deal the SAVED one back over it so Prev/Next walk
     * the order the user was walking — but only when the queue really is the
     * saved one. A fallback to the album is a different queue, and the saved
     * pair would only describe a deal that never happened to it. */
    if (g_queue_kind == kind &&
        (g_settings.resume_order_seed != 0 ||
         g_settings.resume_order_keep == PLAYER_KEEP_QUEUE)) {
        player_reshuffle_with_seed(g_settings.resume_order_seed,
                                   g_settings.resume_order_keep);
    }

    if (g_settings.resume_secs >= RESUME_MIN_SECS) {
        /* Clamped to the track length inside player_seek_to, and a refusal is
         * simply "resumed at 0:00" — never a reason to abandon the track. */
        uint32_t st0 = boot_ms_now();
        (void)player_seek_to(g_settings.resume_secs);
        g_boot_res_seek_ms = boot_ms_now() - st0;
    }
    hal_volume_set(g_volume);

    /* Put the cursor on Now Playing: it is the row this whole feature exists
     * to make one click away, and it only appears while a track is loaded. */
    g_main_sel = MM_NOWPLAYING;
}

/* ---------------------------------------------------------------------------
 * Music > Search (ui/search.c)
 *
 * The module owns the query, the ring, the match and the screen; main.c owns
 * the two things only it knows — WHAT to scan and how a hit reads as a row —
 * and what a chosen hit does, which is always "the thing the ordinary list
 * would have done", so a search result is never a second-class way in.
 * ------------------------------------------------------------------------- */
/* The name each source list is ORDERED by, which is also what its row shows.
 * Songs are addressed by SORTED position, so song hits arrive in title order
 * and the row lookup is one array step. */
static const char *search_name_of(int type, int i)
{
    switch (type) {
    case SEARCH_T_ARTIST:
        return (i >= 0 && i < g_artists_n)   ? g_artists[i].name   : 0;
    case SEARCH_T_ALBUM:
        return (i >= 0 && i < g_albums_n)    ? g_album_key[i]      : 0;
    case SEARCH_T_PLAYLIST:
        return (i >= 0 && i < g_playlists_n) ? g_playlists[i].name : 0;
    case SEARCH_T_SONG:
        return (i >= 0 && i < g_songs_n) ? g_songs[g_song_sorted[i]].title : 0;
    default:
        return 0;
    }
}

/* The key that list is SORTED by: for artists, past a leading "The ", so
 * typing "laroi" ranks The Kid LAROI as a prefix hit rather than burying it
 * under every title with the word in the middle. A pointer INTO the name, as
 * ui/search.h requires. */
static const char *search_artist_key_of(int type, int i)
{
    const char *n = search_name_of(type, i);
    if (!n) return 0;
    return (type == SEARCH_T_ARTIST) ? artist_key(n) : n;
}

static search_source_t g_search_src = {
    { 0, 0, 0, 0 }, search_name_of, search_artist_key_of
};

/* A hit's index only means anything against the list the scan ran over. The
 * library cannot change while the firmware runs, but the hits outlive the
 * screen, so every use of one is bounds-checked rather than trusted. */
static int search_hit_ok(const search_hit_t *h)
{
    int i = (int)h->idx;
    switch (h->type) {
    case SEARCH_T_ARTIST:   return i < g_artists_n;
    case SEARCH_T_ALBUM:    return i < g_albums_n;
    case SEARCH_T_PLAYLIST: return i < g_playlists_n;
    case SEARCH_T_SONG:     return i < g_songs_n;
    default:                return 0;
    }
}

static void sr_copy(char *dst, int cap, const char *src)
{
    int n = 0;
    while (src && src[n] && n < cap - 1) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
}

static int sr_cat(char *dst, int n, int cap, const char *src)
{
    while (src && *src && n < cap - 1) dst[n++] = *src++;
    dst[n] = '\0';
    return n;
}

/* Title, eyebrow and right-hand value for one hit. The eyebrow is the Now
 * Playing "TRACK n OF m" style — FONT_SMALL, muted — because the row has to
 * say WHAT it is before it says which: four kinds share one list here. */
static void search_row_fill(const search_hit_t *h, char *title, char *sub,
                            char *right, int *greyed)
{
    if (!search_hit_ok(h)) {
        return;                        /* left empty; the caller pre-cleared */
    }
    switch (h->type) {
    case SEARCH_T_ARTIST:
        sr_copy(title, SEARCH_ROW_MAX, g_artists[h->idx].name);
        sr_copy(sub,   SEARCH_ROW_MAX, "ARTIST");
        break;
    case SEARCH_T_ALBUM: {
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(g_albums[h->idx].folder, artist, album);
        sr_copy(title, SEARCH_ROW_MAX, g_album_key[h->idx]);
        int n = sr_cat(sub, 0, SEARCH_ROW_MAX, "ALBUM");
        if (artist[0]) {
            n = sr_cat(sub, n, SEARCH_ROW_MAX, " " UI_GLYPH_MIDDOT " ");
            (void)sr_cat(sub, n, SEARCH_ROW_MAX, artist);
        }
        break;
    }
    case SEARCH_T_PLAYLIST:
        sr_copy(title, SEARCH_ROW_MAX, g_playlists[h->idx].name);
        sr_copy(sub,   SEARCH_ROW_MAX, "PLAYLIST");
        break;
    default: {                                         /* SEARCH_T_SONG */
        const lib_song_t *sg = &g_songs[g_song_sorted[h->idx]];
        sr_copy(title, SEARCH_ROW_MAX, sg->title[0] ? sg->title : sg->file);
        int n = sr_cat(sub, 0, SEARCH_ROW_MAX, "SONG");
        if (sg->artist[0]) {
            n = sr_cat(sub, n, SEARCH_ROW_MAX, " " UI_GLYPH_MIDDOT " ");
            (void)sr_cat(sub, n, SEARCH_ROW_MAX, sg->artist);
        }
        if (sg->duration_s) fmt_time(right, sg->duration_s);
        /* A record the disk no longer backs cannot be played. Greyed, and
         * SELECT clicks without acting — the row is still evidence the song
         * was there, which is worth more than hiding it. */
        *greyed = (sg->file_clus == 0);
        break;
    }
    }
}

/*
 * Rerun the match, and say what it cost. The estimate in ui/search.h is
 * 25-55 ms per keystroke on this CPU and nothing has measured it on the
 * device; the UART line is what turns that into a number the bench can read,
 * and it costs one timer read either side of the scan.
 */
static void search_rescan(void)
{
    g_search_src.count[SEARCH_T_ARTIST]   = g_artists_n;
    g_search_src.count[SEARCH_T_ALBUM]    = g_albums_n;
    g_search_src.count[SEARCH_T_PLAYLIST] = g_playlists_n;
    g_search_src.count[SEARCH_T_SONG]     = g_songs_n;
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    search_scan(&g_search, &g_search_src);
    uint32_t us = mmio_read32(USEC_TIMER_ADDR) - t0;
    uart_puts("core: search ");  uart_dec(g_search.qlen);
    uart_puts(" chars ");        uart_dec(g_search.total);
    uart_puts(" hits ");         uart_dec((int)(us / 1000u));
    uart_puts(" ms\n");
}

static void search_render_cur(void)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    search_render(&g_search, search_row_fill);
}

/*
 * Load album `ai`'s tracklist into g_browse, its metadata and its art, and
 * point the album list at it. Unfiltered, so there is no synthetic All Songs
 * row and the list row for album `ai` IS `ai` — the album the user picked is
 * the one the list would land on if they ever backed out to it. The depth is
 * borrowed for the read the way resume_open_album does it: browse_collect
 * only lists files at depth 1.
 */
static void search_load_album(fat32_t *fs, int ai)
{
    g_artist_filter[0] = '\0';
    albumview_build(0);
    g_br_sel    = ai;
    g_br_accum  = 0;
    g_dir_depth = 1;
    split_artist_album(g_albums[ai].folder, g_album_artist, g_album_title);
    browse_load(fs, g_albums[ai].clus);
    detail_load_meta(fs);
    g_det_sel = g_det_accum = 0;
}

/*
 * Start a song hit in its ALBUM, at that song. 1 when the queue is running.
 *
 * The album and not the six-thousand-row Songs view: "play this one" means
 * this one in its record, it resumes through the existing RESUME_KIND_ALBUM
 * path, and it skips the LOADING SONGS bar. That reuses g_browse, which is
 * only safe while no tracklist is on the stack underneath — Search is
 * reachable from Music alone, so it is; the UART line is what will say so if
 * that ever stops being true.
 */
static int search_play_song(fat32_t *fs, const search_hit_t *h)
{
    const lib_song_t *sg = &g_songs[g_song_sorted[h->idx]];
    if (!sg->file_clus) {
        return 0;                       /* greyed: nothing on the disk to play */
    }
    for (int i = 0; i < g_scr_n; i++) {
        if (g_scr[i] == SCR_BROWSER) {
            uart_puts("core: search play under a browser, tracklist lost\n");
            break;
        }
    }
    int saved_depth = g_dir_depth;
    g_dir_depth = 1;                /* browse_collect lists FILES at depth 1 */
    browse_load(fs, sg->dir_clus);
    g_dir_depth = saved_depth;
    /* The row is the file the record bound to: same cluster, never the name
     * (see resume_open_album, which binds the same way). */
    int row = -1;
    for (int i = 0; i < g_browse_n; i++) {
        if (!g_browse[i].is_dir && g_browse[i].clus == sg->file_clus) {
            row = i;
            break;
        }
    }
    if (row < 0) {
        return 0;                   /* the folder no longer holds the file */
    }
    g_queue_kind = RESUME_KIND_ALBUM;
    g_queue_seed = 0;
    player_play_queue(g_browse, g_browse_n, row, g_art_clus, g_art_size);
    return 1;
}

/*
 * Act on the selected hit. Every case is the block the ordinary list runs, so
 * a result behaves exactly like the row it stands for. Returns 1 when Now
 * Playing was pushed (the caller arms its first-paint flag).
 */
static int search_open_hit(fat32_t *fs)
{
    if (g_search.sel < 0 || g_search.sel >= g_search.nhit) return 0;
    const search_hit_t *h = &g_search.hit[g_search.sel];
    if (!search_hit_ok(h)) return 0;

    switch (h->type) {
    case SEARCH_T_ARTIST: {
        int k = 0;
        for (; g_artists[h->idx].name[k] && k < NAME_MAX; k++) {
            g_artist_filter[k] = g_artists[h->idx].name[k];
        }
        g_artist_filter[k] = '\0';
        g_dir_depth = 0;
        albumview_build(g_artist_filter);
        albumlist_queue_chips();
        g_br_sel = g_br_accum = 0;
        g_br_from_search = 0;           /* the album list IS the screen here */
        scr_push(SCR_BROWSER);
        return 0;
    }
    case SEARCH_T_ALBUM:
        search_load_album(fs, h->idx);
        g_br_from_search = 1;
        scr_push(SCR_BROWSER);
        return 0;
    case SEARCH_T_PLAYLIST:
        playlist_open(fs, h->idx);
        scr_push(SCR_PLAYLIST);
        return 0;
    default:                                           /* SEARCH_T_SONG */
        if (!search_play_song(fs, h)) {
            return 0;
        }
        hal_volume_set(g_volume);   /* re-apply over the codec re-init */
        scr_push(SCR_NOWPLAYING);
        return 1;
    }
}

/*
 * PLAY on a result row — the gesture, not the button. Every case is what PLAY
 * does on the list that row came from: an artist plays their whole
 * discography, an album plays from its first track, a playlist from its
 * first, a song plays in its album. Returns PLAY_TAP_STARTED with the queue
 * running and the push left to play_tap_start's shared tail.
 */
static play_tap_t search_play_hit(fat32_t *fs)
{
    if (g_search.sel < 0 || g_search.sel >= g_search.nhit) return PLAY_TAP_PASS;
    const search_hit_t *h = &g_search.hit[g_search.sel];
    if (!search_hit_ok(h)) return PLAY_TAP_PASS;

    switch (h->type) {
    case SEARCH_T_ARTIST: {
        int k = 0;
        for (; g_artists[h->idx].name[k] && k < NAME_MAX; k++) {
            g_artist_filter[k] = g_artists[h->idx].name[k];
        }
        g_artist_filter[k] = '\0';
        songview_build(-1, g_artist_filter);
        if (library_play_song(fs, 0) < 0) return PLAY_TAP_CONSUMED;
        break;
    }
    case SEARCH_T_ALBUM:
        search_load_album(fs, h->idx);
        if (g_browse_err != 0 || g_browse_n == 0) {
            /* Unreadable or empty: put the tracklist screen up with the reason,
             * exactly as SELECT would, and do not ALSO toggle a transport that
             * never started. */
            g_br_from_search = 1;
            scr_push(SCR_BROWSER);
            return PLAY_TAP_CONSUMED;
        }
        g_queue_kind = RESUME_KIND_ALBUM;
        g_queue_seed = 0;
        player_play_queue(g_browse, g_browse_n, 0, g_art_clus, g_art_size);
        g_dir_depth = 0;            /* no BROWSER under this: Search is */
        break;
    case SEARCH_T_PLAYLIST:
        playlist_open(fs, h->idx);
        if (playlist_play(0) < 0) {
            scr_push(SCR_PLAYLIST); /* the screen that says why, as SELECT */
            return PLAY_TAP_CONSUMED;
        }
        break;
    default:                                           /* SEARCH_T_SONG */
        /* Greyed, or the folder no longer holds the file: the press was aimed
         * at this row and declined, and the player was never touched. */
        if (!search_play_song(fs, h)) return PLAY_TAP_CONSUMED;
        break;
    }
    return PLAY_TAP_STARTED;
}

/* Render whatever screen is on top of the stack into the framebuffer (no
 * present) — used to paint context behind the Hold banner. */
static void paint_current_screen(void)
{
    g_mq.active = 0;                       /* a fresh paint re-registers any marquee */
    switch (scr_cur()) {
    case SCR_MENU:    main_menu_render();  break;
    case SCR_MUSIC:   music_menu_render(); break;
    case SCR_ARTISTS: artists_render(g_artist_sel); break;
    case SCR_SONGS:   songs_render(g_song_sel);     break;
    case SCR_GENRES:  genres_render(g_genre_sel);   break;
    case SCR_PLAYLISTS: playlists_render(g_pl_sel); break;
    case SCR_PLAYLIST:  playlist_render(g_plt_sel); break;
    case SCR_OTG:       otg_render(g_otg_sel);      break;
    case SCR_SEARCH:    search_render_cur(); break;
    case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel); break;
    case SCR_QUEUE:   queue_render(g_queue_sel); break;
    case SCR_SETTINGS: settings_render_cur(); break;
    case SCR_NOWPLAYING:
        nowplaying_render(player_track_name(), player_elapsed_s(),
                          player_total_s(), player_buf_pct());
        break;
    case SCR_BATTERY:
        battery_modal_paint();
        break;
    case SCR_CHARGING:
        charging_screen_paint();
        break;
    }
}

/*
 * Quiesce and enter PMU deep-sleep standby — the true "off" (holding PLAY
 * past ~5 s, the suspend timeout, or the battery policy's SHUTOFF edge).
 * A button press wakes the device by re-running the boot path (a cold boot
 * of the firmware, not a resume).
 *
 * Everything that draws is put away BEFORE the rail cut, in the order the
 * hardware wants: the transport and the codec (hal_audio_close powers the
 * WM8758 down and gates the audio clocks), then the settings write while the
 * drive is still spinning, then the drive itself (cache flushed, heads
 * parked, spun down — 04-ata.md: "safe to cut power after this returns"),
 * then the panel (black frame, backlight off, LCD_SLEEP). The PMU write is
 * last. The settings write goes through the normal battery gate: below the
 * disk-safe line it is refused, and the DISKSAFE-edge flush that already
 * happened is what persists (see CFG_COMMIT_LAST).
 *
 * RETURNS -1 IF THE PMU REFUSED. power_standby() gives up after a bounded
 * number of I2C retries rather than hang, and this used to fall into a
 * for(;;) regardless — a dark, dead device, reached by the user asking to
 * turn it off. Now the refusal is recovered here: panel woken, the current
 * screen repainted (the present absorbs the panel init), backlight restored,
 * and the caller carries on. The player has been STOPPED by then, not paused
 * — that is the price of the refusal, and the caller must not resume it.
 * On the device this path has never been seen to fire; it is the I2C-wedged
 * case, and the recovery sequence is UNVERIFIED on hardware.
 */
static void suspend_lowpower_leave(void);

/* DEVICE RESULT 2026-09-13: OFF. The idle-path twin (PANEL_SLEEP_AT_IDLE)
 * came back WHITE on the first wake on the real 5.5G — the LCD_SLEEP →
 * lcd_wake → absorbed first present sequence does not bring the panel back
 * as modelled. Same pair here, so off until that is understood on the
 * bench. The LED is still off across a suspend; the panel merely holds a
 * black frame instead of being slept. */
#ifndef SUSPEND_PANEL_SLEEP
#define SUSPEND_PANEL_SLEEP 0
#endif

int enter_standby(void)
{
    /* If a suspend brought us here (its hold-escalation, its 30-minute
     * escalation, or the SHUTOFF edge inside its battery sample), the PLL
     * is parked, the tick is at 10 Hz and SER0/PWM/I2C are gated. Every
     * step below — the codec's I2C writes, the settings write on a boosted
     * ATA transfer, the BCM frame, and the PMU's own I2C command — was
     * calibrated at the PLL operating points, so come back first. No-op
     * from the main loop. */
    suspend_lowpower_leave();

    /* BEFORE player_stop(): once the transport is torn down there is no track
     * name and no elapsed clock left to record. */
    resume_capture();
    player_stop();
    hal_audio_close();                    /* codec rails off, audio clocks gated */
    settings_commit(1);                   /* persist while the drive still spins */
    otg_commit(CFG_COMMIT_FORCE);         /* ...and the On-The-Go list with it */
    uart_puts("core: standby: entering\n");
    evlog_commit(CFG_COMMIT_FORCE);       /* the log's last block, FINAL; same
                                           * gate — refused below disk-safe, as
                                           * the settings commit above is    */
    if (!ata_is_parked()) {
        ata_standby();                    /* flush + park + spin down. Not
                                           * ata_sleep(): the rail cut follows
                                           * and STANDBY is the device-proven
                                           * path; a suspend has already slept
                                           * it (parked) by the time it gets here */
    }
    console_clear(0xFFFF);                /* blank BEFORE the power cut so no */
    lcd_present_fb(console_framebuffer()); /* stale colour lingers on the panel.
                                           * WHITE, not black: with the LED off a
                                           * black frame on this transflective
                                           * panel is a dark field with the pixel
                                           * grid showing in ambient light; white
                                           * is what "off" looks like (device,
                                           * 2026-09-13)                        */
    cpu_wait_ms(80);                      /* let the BCM push the frame        */
    backlight_set(0);
#if SUSPEND_PANEL_SLEEP
    lcd_sleep();                          /* panel off; the BCM stays alive   */
#endif
    (void)power_standby();                /* PMU cuts power — normally no return */

    /*
     * Still here: the PMU never took the command. Bring the device back to a
     * usable state rather than leave it dark. Wake the panel and push ONE
     * frame BEFORE the backlight: that first present is the one the BCM
     * answers with its ~500 ms panel init, and lighting the LED over it is
     * the white flash (02-lcd.md).
     */
    uart_puts("core: standby REFUSED by the PMU; staying up\n");
    lcd_wake();
    paint_current_screen();
    lcd_present_fb(console_framebuffer());
    backlight_set(g_settings.backlight_bright);
    g_standby_refused = 1;
    return -1;
}

/*
 * Suspend: the seamless "off". Keeps the CPU + RAM alive so wake RESUMES the
 * running firmware instantly (no cold boot) — and puts everything else away:
 *
 *   audio     paused; the codec powers itself down through player_pump()'s
 *             persistent-pause timeout, which the idle loop keeps calling;
 *   settings  written now (forced), with the resume position;
 *   drive     ata_sleep(): cache flushed, heads parked, platters down, then
 *             SLEEP so the interface logic is off too — a reset wakes it;
 *   panel     black frame, backlight off, then LCD_SLEEP (SUSPEND_PANEL_SLEEP;
 *             the BCM stays powered so no firmware re-upload on wake);
 *   SoC       boost released, then suspend_lowpower_enter(): SER0, PWM0 and
 *             I2C clocks gated in DEV_EN (each re-gates itself on use),
 *             TIMER1 at 10 Hz, and the PLL disabled and unpowered with the
 *             bus on the 24 MHz crystal (kernel/clock.c clock_suspend).
 *
 * While suspended the loop samples the battery on the main loop's 5 s
 * cadence and runs the same DISKSAFE / SHUTOFF policy, so a forgotten device
 * flushes and powers off cleanly instead of deep-discharging to the PMU's
 * hard cut; and on battery it escalates to a real PMU standby on its own
 * after SUSPEND_TO_STANDBY_US. Holding the trigger PLAY past ~5 s escalates
 * at once. `play_down_us` is when the hold began, for that escalation.
 *
 * Any button wakes it: reset + spin the drive up, wake the panel, repaint,
 * present (that present retires the panel's wake-init before returning),
 * THEN backlight, then resume — only if the headphone jack is not known to
 * be empty. A refused PMU standby from any of the escalations falls through
 * this same wake path with the player stopped and nothing to resume.
 *
 * What still draws: the CPU at 24 MHz and the SDRAM (the wake is what they
 * buy — the doc's 32 kHz point is for a core that has stopped), the OPTO
 * block (the wake source), the BCM, and whatever the ROM's undocumented
 * DEV_EN bits (USB/FireWire/IDE) keep clocked. That is why the escalation
 * exists. NOT MEASURED on the device: the suspend draw before or after any
 * of this, whether the panel comes back from LCD_SLEEP
 * (SUSPEND_PANEL_SLEEP has the rollback), the drive's post-SLEEP reset
 * wake, and whether the I2C controller and the wheel come back cleanly
 * from a gate / a crystal-clocked spell — all first-flash items.
 */
/*
 * Put the LCD PANEL to sleep for the suspend, not just the backlight.
 *
 * lcd_sleep() issues the BCM's LCD_SLEEP (panel driver off; the BCM itself
 * stays powered and bootstrapped, so no firmware re-upload is needed). The
 * hazard is on the way back: the first LCD_UPDATE after a wake carries a
 * ~500 ms panel init, and light behind the panel before that update has
 * retired is the solid-white screen (02-lcd.md). The wake path below does
 * lcd_wake(); paint; present; backlight in that order, and the present does
 * not return until the init has retired (hal/hw/lcd.c bcm_frame_commit) —
 * which is what makes this safe to ship at 1.
 *
 * UNVERIFIED ON THE DEVICE as of 2026-09-13. If the screen comes back WHITE
 * after a suspend, set this to 0: the panel then stays driven-black for the
 * suspend exactly as before (backlight off only), and nothing else changes.
 */

/*
 * How long a suspend may last on battery before it escalates to a real PMU
 * standby (enter_standby). Suspend keeps the CPU, RAM, PLL and the 100 Hz
 * tick alive so that wake is instant; that is the right trade for "off for
 * a minute", and the wrong one for "off overnight", where the same draw
 * just takes the cell to the PMU's hard cut with nothing saved. Not applied
 * while on external power: a docked device can sit suspended for as long
 * as it likes. The cost of escalating is a cold boot on the next wake.
 */
/* How long PLAY must stay held AFTER the screen went dark before the hold
 * escalates to a PMU power-off (in addition to ~5 s from the down-edge). */
#ifndef SUSPEND_ESCALATE_DARK_US
#define SUSPEND_ESCALATE_DARK_US  2500000u                /* 2.5 s */
#endif

#ifndef SUSPEND_TO_STANDBY_US
#define SUSPEND_TO_STANDBY_US  (30u * 60u * 1000000u)     /* 30 minutes */
#endif

/* Idle-loop period while suspended, and the tick rate the loop runs under.
 * Nothing in the loop needs to be prompt: the tick samples the wheel into
 * the latch regardless, so a press is seen within one tick period, and the
 * battery sample is on its own 5 s cadence. The loop period matches the
 * tick period, so a suspended core wakes ~20 times a second at most (once
 * for the tick, once for the countdown) instead of ~110; a press shorter
 * than one tick period can fall between samples, which is why "hold any
 * button" is the wake gesture. */
#define SUSPEND_IDLE_MS        100u
#define SUSPEND_TICK_HZ        10u

/*
 * The suspend operating point, entered once the drive, panel and backlight
 * are down (kernel/clock.c, kernel/timer.c):
 *
 *   clock_gate_suspend   DEV_EN: SER0, PWM0 and I2C clocks off — each block
 *                        re-gates itself on its next use, so a UART line,
 *                        an I2C transaction or a click still works;
 *   timer_set_rate(10)   TIMER1 at 10 Hz (the tick counter keeps its 10 ms
 *                        unit; only the wheel sampling coarsens);
 *   clock_suspend        bus onto the 24 MHz crystal, PLL disabled and
 *                        unpowered. Refused (logged) if a boost is still
 *                        held — nothing in this file should be holding one
 *                        by then.
 *
 * In that order, so the core clock is the last thing to move. Left in the
 * reverse order. Both are idempotent through g_suspend_lp so the battery
 * sample, the escalations and the wake path can each call them without
 * caring who went first — and so timer_set_rate is not re-issued on every
 * loop pass, which at a 100 ms period would restart the countdown before
 * it ever fired.
 */
static int g_suspend_lp;

/*
 * DEVICE BISECT, 2026-09-13. With all three of these on, a suspend went dark
 * and never woke (no button, no charger; Menu+Select only), and so did the
 * 5 s hold — its escalation runs from inside the suspend loop, so a loop
 * that cannot see the wheel never escalates either. Panel sleep was already
 * off for that flash. Each piece is switchable so a flash can turn one on
 * at a time; all OFF is the known-good shape (the 2026-09-10 suspend, plus
 * ATA SLEEP).
 *
 * RESULT (three flashes, same evening): all off — wakes. Tick + gates on,
 * PLL off — dark. Gates ONLY — dark. So the CLOCK GATES are what break the
 * wake: the wheel was still alive when the PLAY release was seen (gates
 * already applied), so the kill is later — the 5 s battery cycle un-gates
 * and re-gates SER0/PWM/I2C (the I2C re-gate is a full reset-pulse
 * bring-up), and something in that round trip stops the wheel or the tick.
 * Not yet split per block. The 10 Hz tick was never tested ALONE; the PLL
 * park never tested at all. Both stay off until the on-disk event log can
 * show what the loop did last. Ship: all three 0.
 */
#ifndef SUSPEND_PARK_PLL
#define SUSPEND_PARK_PLL     0
#endif
#ifndef SUSPEND_SLOW_TICK
#define SUSPEND_SLOW_TICK    0
#endif
#ifndef SUSPEND_GATE_CLOCKS
#define SUSPEND_GATE_CLOCKS  0
#endif

static void suspend_lowpower_enter(void)
{
    if (g_suspend_lp) {
        return;
    }
    g_suspend_lp = 1;
#if SUSPEND_GATE_CLOCKS
    clock_gate_suspend();
#endif
#if SUSPEND_SLOW_TICK
    timer_set_rate(SUSPEND_TICK_HZ);
#endif
#if SUSPEND_PARK_PLL
    if (clock_suspend() != 0) {
        /* The UART re-gates itself for this line. */
        uart_puts("core: suspend: PLL park refused (boost held or DMA live)\n");
    }
#endif
}

static void suspend_lowpower_leave(void)
{
    if (!g_suspend_lp) {
        return;
    }
    g_suspend_lp = 0;
#if SUSPEND_PARK_PLL
    clock_resume();
#endif
#if SUSPEND_SLOW_TICK
    timer_set_rate(HZ);
#endif
#if SUSPEND_GATE_CLOCKS
    clock_gate_resume();
#endif
}

static void suspend_to_ram(uint32_t play_down_us)
{
    wheel_event_t drain;
    uint32_t suspend_t0 = mmio_read32(USEC_TIMER_ADDR);
    int was_playing = player_active() && !player_paused();
    if (was_playing) {
        player_pause();                   /* silence + stop feeding the disk */
    }
    /* Last chance to persist: after this the drive is parked and the user may
     * never wake the device again (battery pull, dead cell). Forced, so a
     * change made 1 s ago is not lost to the debounce. The position goes in
     * the same write — the pause above froze the elapsed clock, so this is the
     * exact second the user stopped listening. */
    resume_capture();
    settings_commit(1);
    otg_commit(CFG_COMMIT_FORCE);
    /* The log's last chance too, in the same breath: what is pending goes
     * out as a FINAL block while the drive is still up (the forced flush
     * wakes it if a parked-drive commit above did not). Narrated first, so
     * the block records that a suspend was entered. */
    uart_puts("core: suspend: entering\n");
    evlog_commit(CFG_COMMIT_FORCE);
    cpu_unboost();                        /* the boost refcount is >=1 here (we are
                                           * entered from BL_FULL), so without this
                                           * the "sleeping" device holds the 80 MHz
                                           * operating point for the whole suspend */
    /* DEVICE 2026-09-13: STANDBY, not SLEEP. Every short-hold suspend today
     * went dark and "never woke" while the 5 s power-off (which never
     * touches the drive) came back fine. SLEEP needs a bus reset to recover
     * and the wake path issues one BEFORE repainting; if this drive does
     * not answer it, the wake sits in the 31 s reset budget with the screen
     * dark. STANDBY is the device-proven park; a plain read spins it up. */
#ifndef SUSPEND_ATA_SLEEP
#define SUSPEND_ATA_SLEEP 0
#endif
#if SUSPEND_ATA_SLEEP
    ata_sleep();                          /* flush, park, spin down, interface off */
#else
    if (!ata_is_parked()) {
        ata_standby();                    /* park + spin down, interface stays up */
    }
#endif
    /* Clear to black BEFORE cutting the backlight, so the transflective panel
     * doesn't faintly ghost the last UI in ambient light while asleep. Wake
     * repaints the real screen while the backlight is still off (below), so the
     * blank is never seen as a flash. WHITE, not black: with the LED off a
     * black frame shows as a dark field with the pixel grid visible in
     * ambient light — "random black pixels" (device, 2026-09-13); a white
     * frame is what an iPod looks like when it is off. */
    console_clear(0xFFFF);
    lcd_present_fb(console_framebuffer());
    backlight_set(0);
#if SUSPEND_PANEL_SLEEP
    lcd_sleep();                          /* panel driver off; drains the black
                                           * frame first, then LCD_SLEEP     */
#endif
    suspend_lowpower_enter();             /* gates, 10 Hz tick, PLL parked */

    /* Wait for the trigger PLAY hold to release (so it can't instantly wake us).
     * Held past ~5s total => a real power-down instead. If the PMU refuses
     * that, enter_standby() has already stopped the player and repainted:
     * skip the idle wait and fall through to the wake path, which finishes
     * the job (release-wait, re-boost, drive spin-up) without resuming. */
    /* Escalation is timed from BOTH the down-edge (~5 s of hold, the
     * documented gesture) AND from the moment the screen went dark. The
     * teardown above is not instant: a pending settings change on a parked
     * drive is a spin-up (seconds) + write + FLUSH + SLEEP before the
     * backlight drops. Timed from the down-edge alone, "hold until it goes
     * dark, then let go" — the natural gesture — arrived here already past
     * 5 s with PLAY down for one more tick, and turned the sleep the user
     * asked for into a PMU power-off. */
    uint32_t dark_us = mmio_read32(USEC_TIMER_ADDR);
    int standby_refused = 0;
    int jack_pulled = 0;              /* the headphones came out while asleep */
    while (clickwheel_buttons() & WHEEL_BTN_PLAY) {
        uint32_t nowh = mmio_read32(USEC_TIMER_ADDR);
        if ((uint32_t)(nowh - play_down_us) > 5000000u &&
            (uint32_t)(nowh - dark_us)      > SUSPEND_ESCALATE_DARK_US) {
            if (enter_standby() != 0) {   /* true off (PMU) — normally no return */
                standby_refused = 1;
                was_playing     = 0;      /* stopped, not paused: nothing to resume */
                break;
            }
        }
        /* See the wake loop below for why this drain is load-bearing. */
        while (clickwheel_get_event(&drain)) { }
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }   /* drop the trigger's latched events */

    /* Low-power idle until any button is pressed. The (now 10 Hz) tick keeps
     * sampling the wheel into the latch through each cpu_wait. A press is
     * seen two ways: the live held-button state below, and any button
     * DOWN-EDGE the drain hands back — at 10 Hz a press can start and end
     * between two of the loop's own looks at the live state, but the tick
     * sampler latched its edge, and dropping that on the floor would be a
     * wake the user made and the device ignored. */
    /* Reached the idle loop: the last thing the log can say before the
     * device goes quiet, and the first thing a stuck suspend is missing.
     * Not flushed (the drive is asleep); it rides out in the wake's block.
     * The line un-gates SER0 to go out — re-gate it, or the suspend spends
     * the night with the UART clocked. */
    uart_puts("core: suspend: idle loop\n");
#if SUSPEND_GATE_CLOCKS
    if (g_suspend_lp) {
        uart_clock_suspend();
    }
#endif
    int pressed = 0;
    while (!standby_refused && !pressed && clickwheel_buttons() == 0) {
        /*
         * DRAINING HERE IS WHAT MAKES THE DEVICE WAKE AT ALL.
         *
         * clickwheel_buttons() returns a CACHED value that only the tick
         * sampler refreshes. When the Hold switch goes off, the sampler gates
         * the OPTO block's clock back on but deliberately defers the reset/
         * config sequence out of ISR context by setting s_need_bringup — and
         * then bails on every subsequent tick until something clears it. The
         * only thing that clears it is clickwheel_get_event().
         *
         * So a user who suspends and then flips Hold on and off again (the
         * standard reflex after "turning it off") left the cached button state
         * pinned at 0 forever: this loop span on a value that could never
         * change, dark and unresponsive, with a hardware reset the only escape.
         * The normal main loop never hit this because it drains every pass.
         *
         * Draining does not clear the cached state, so the loop condition is
         * unaffected — it only lets the deferred bring-up actually run.
         * (A Hold edge is an event with no buttons: not a wake.)
         */
        while (clickwheel_get_event(&drain)) {
            if (drain.buttons != 0) {
                pressed = 1;
            }
        }
        if (pressed) {
            break;                /* wake now, not after another 100 ms halt */
        }
        /*
         * Pump while suspended, so the codec actually powers down.
         *
         * The pause above stops the DMA but leaves the WM8758 fully powered —
         * PLL, VMID, DACs, headphone amps — and the thing that shuts it off is
         * the persistent-pause timeout inside player_pump(). This loop never
         * called pump, so a device "asleep" via Play-hold held the codec live
         * for the entire suspend, which is the most expensive way to do
         * nothing that this firmware is capable of.
         *
         * Safe to call here: pump's paused branch does the codec-off check and
         * returns immediately. No decode, no disk, no ring work — which
         * matters, because the drive is parked by this point and waking it
         * would defeat the whole exercise.
         */
        player_pump();
        /* Nothing above may hold a boost, but cpu_boost() from the crystal
         * un-parks the PLL and cpu_unboost() leaves it at 30 MHz — so a
         * future disk touch in this loop would silently spend the rest of
         * the suspend off the crystal. Re-park if that happened. */
#if SUSPEND_PARK_PLL
        if (g_suspend_lp && cpu_frequency() != CPUFREQ_DEFAULT) {
            (void)clock_suspend();
        }
#endif
        /* DEVICE 2026-09-13: the guard above used to run UNCONDITIONALLY —
         * so the PLL park executed on every suspend of every image that
         * evening, including the ones with SUSPEND_PARK_PLL 0, and the wake
         * side (which honours the switch) never un-parked it. Every "all
         * off" bisect step was therefore really "PLL park on". */

        /*
         * Watch the battery. battery_refresh() only ever ran from the main
         * loop, so a suspended device took no samples at all: it discharged
         * straight past the DISKSAFE and SHUTOFF lines to the PMU's hard cut,
         * with nothing flushed and no goodbye. Same call, same 5 s cadence,
         * same policy — the DISKSAFE edge makes its last write (which wakes
         * the slept drive, so put it back to sleep afterwards if the handler
         * left it up), and the SHUTOFF edge powers the device off through
         * enter_standby(). If the PMU refuses THAT, enter_standby has already
         * stopped the player and relit the screen: leave the loop the way a
         * refused hold-escalation does, and do not resume.
         *
         * The sample runs at the normal operating point: the clocks come
         * back for it and are parked again after. The gated blocks would
         * re-gate themselves, but the sample's consequences — a DISKSAFE
         * settings write on a boosted ATA transfer, a SHUTOFF power-off
         * through enter_standby — are paths calibrated at the PLL clocks,
         * and the log line goes out on the boot-tested UART setup. Two PLL
         * relocks per 5 s is a few hundred microseconds of duty.
         */
        if (battery_due()) {
            suspend_lowpower_leave();
            (void)battery_refresh(0);
            if (g_standby_refused) {
                standby_refused = 1;
                was_playing     = 0;
                break;
            }
#if SUSPEND_ATA_SLEEP
            if (!ata_is_slept()) {
                ata_sleep();        /* the handler re-parks with STANDBY only */
            }
#else
            if (!ata_is_parked()) {
                ata_standby();
            }
#endif
            suspend_lowpower_enter();
        }

        /*
         * Keep the jack watcher fed (one GPIO read through the debouncer), so
         * a plug pulled while the device sleeps is SEEN rather than inferred
         * from the level at wake. `was_playing` is the transport's state as
         * far as this feature is concerned — the pause above was the sleep's,
         * not the listener's — so a pull here answers PAUSE, and answering it
         * by dropping `was_playing` is how the wake below declines to resume.
         *
         * This is what makes "pulled and re-inserted while it slept" stay
         * paused: without it the wake sees a seated plug and cannot tell that
         * anything happened. Nothing is printed from inside the park (SER0's
         * clock may be gated); the wake says it once the clocks are back.
         */
        uint32_t now = mmio_read32(USEC_TIMER_ADDR);
        if (jackwatch_feed(&g_jack, hal_headphones_present(), was_playing,
                           now) == JACKWATCH_PAUSE) {
            was_playing = 0;
            jack_pulled = 1;
        }

        /*
         * Escalate. Past SUSPEND_TO_STANDBY_US on battery this is no longer
         * a short "off": trade the instant wake for a real power-down before
         * the cell is spent. Checked every pass, so a device unplugged after
         * the deadline escalates on the next one.
         */
        if (!power_is_external() &&
            (uint32_t)(now - suspend_t0) > SUSPEND_TO_STANDBY_US) {
            if (enter_standby() != 0) {   /* normally no return */
                standby_refused = 1;
                was_playing     = 0;
                break;
            }
        }
        cpu_wait_ms(SUSPEND_IDLE_MS);
    }
    /* Swallow the wake press so it isn't also acted on as navigation. */
    while (clickwheel_buttons() != 0) {
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }

    /* Back to the normal operating point FIRST — PLL at 30 MHz, 100 Hz tick,
     * SER0/PWM/I2C clocks restored — so the boost below is an ordinary
     * 30 -> 80 MHz switch and everything after it runs on the clocks it
     * was calibrated at. No-op if a refused standby already brought us
     * back, or if the park was never entered. */
    suspend_lowpower_leave();

    /* Re-boost BEFORE the drive and the repaint: cpu_boost/cpu_unboost are
     * refcounted, so this pairs with the unboost on the way in and keeps the
     * count balanced (an unmatched unboost would drive it negative the next
     * time the idle path unboosts). It also puts the ATA read and the render
     * back at 80 MHz, which is where their timing was calibrated. */
    cpu_boost();
    ata_wakeup();                         /* reset + spin the drive back up before any read */
    uart_puts(standby_refused ? "core: suspend: wake (standby refused)\n"
                              : "core: suspend: wake\n");
    /*
     * The clock, before anything paints one. The USEC_TIMER cannot be trusted
     * across a suspend — the PLL was parked and the tick dropped to 10 Hz — so
     * the software clock is RE-ANCHORED from the chip rather than carried over
     * a gap it has no way to measure. The RTC itself kept counting: it is in
     * the PMIC's always-on domain (docs/hw/06-power.md).
     */
    clock_resync("wake", 0);
    g_clock_min = 0;                      /* the strip repaints with the screen */
    /*
     * Panel back, in the only order that is safe: wake the panel driver,
     * render the real screen while everything is still dark, present it —
     * this present carries the BCM's ~500 ms panel init and does not return
     * until that has retired — and ONLY THEN light the backlight. Light any
     * earlier and the init shows as a white flash; on the old absorb
     * ordering it could latch white for good. lcd_wake() is a no-op when
     * the panel was not slept (SUSPEND_PANEL_SLEEP 0, or a refused standby
     * that already woke it), and the present is then the ordinary one.
     */
    lcd_wake();
    paint_current_screen();               /* render the real screen while dark... */
    lcd_present_fb(console_framebuffer()); /* ...retire the panel init...     */
    backlight_set(g_settings.backlight_bright);  /* ...then light up straight to it */
    /*
     * The jack, once, for both of the decisions below. The watcher was fed
     * through the whole suspend, so it has already accounted for anything
     * that happened; priming it here is the backstop for the paths that
     * leave that loop without a last feed (a refused standby, a battery
     * verdict), and it costs nothing when the level has not moved.
     */
    int hp = hal_headphones_present();
    jackwatch_prime(&g_jack, hp);
    if (jack_pulled) {
        /* Said here rather than inside the park: SER0's clock may be gated
         * down there. */
        uart_puts("core: jack out during suspend, staying paused\n");
    }
    if (was_playing) {
        /*
         * Resume only into a seated plug. -1 (detect not yet trusted on this
         * device, hal/hw/headphone.h) keeps today's behaviour: resume. 0
         * leaves it paused, one PLAY away.
         */
        if (hp != 0) {
            player_resume();
        }
    }
}

/*
 * Panel sleep at IDLE: when the backlight times fully off, put the LCD panel
 * to sleep too (LCD_SLEEP), not just the LED. The panel driver is the second
 * largest draw on a dark, playing device after the LED itself.
 *
 * HISTORY. The first attempt left the screen solid WHITE until a reboot — the
 * worst failure on a device whose only debug channel is that screen. The
 * mechanism (docs/hw/02-lcd.md): the first LCD_UPDATE after LCD_SLEEP makes
 * the BCM re-run its internal panel init, allowed up to 500 ms. At the time the
 * commit handshake budgeted ~2 ms and RE-KICKED LCD_UPDATE 16 times inside that
 * window, every later present streamed a fresh 150 KB frame into the
 * in-progress init, and the backlight was lit at the input site before any of
 * it — so the init never completed and the BCM latched. The doc names the
 * symptom: "If we wake the backlight before the first update completes, the
 * user sees a 500 ms white flash."
 *
 * WHAT IS TRUE NOW, and why this is on:
 *   - hal/hw/lcd.c bcm_frame_commit: the first present after lcd_wake()
 *     issues its LCD_UPDATE and then WAITS, wall-clock bounded and with NO
 *     re-kick, until that update has retired. The present returns with the
 *     panel init done (hw-lcd-present: test_lcd_present_post_wake).
 *   - While slept, every present is REFUSED and counted (lcd_presents_refused;
 *     test_lcd_present_refused_reported), so nothing can stream into the
 *     sleeping BCM or its wake-init. In this loop every painter — the render
 *     block, the two bar animators, the marquee, the lock plate, the toast —
 *     is gated on bl_state != BL_OFF anyway, so on the idle path nothing even
 *     tries; the count is the proof of that on the device.
 *   - The wake is ONE block (search "Panel wake"), in the only order that is
 *     safe and the same one suspend_to_ram and enter_standby use: lcd_wake();
 *     paint the current screen while everything is still dark; present that
 *     FULL frame (it absorbs the init); THEN backlight_set(). The input sites
 *     do not light the LED themselves when the panel is slept — they only
 *     move bl_state, and the wake block does the rest before anything else in
 *     the pass can present. The wake frame is full by construction, so a
 *     partial-paint cache (g_lp, np_last) that went stale while dark cannot
 *     put a two-row partial on top of pixels the BCM no longer has.
 *   - Two owners compose: a panel slept here and then suspended (hold PLAY
 *     from dark) is slept once — lcd_sleep is idempotent — and woken once by
 *     suspend's own wake; the loop clears panel_slept on the way back so it
 *     does not wake twice. Likewise a refused PMU standby.
 *   - lcd_recover() (a real bcm_init) exists for a wake that never retires,
 *     but stays compiled out (LCD_RECOVER_ON_WAKE 0) until it has run on
 *     silicon; a failed absorb is reported (lcd_bcm_timeouts) and the frame
 *     goes out anyway, which is what the old code did on every frame.
 *
 * UNVERIFIED ON THE DEVICE as of 2026-09-13; suspend's panel sleep
 * (SUSPEND_PANEL_SLEEP) is the same lcd_sleep/lcd_wake pair and is equally
 * unflashed. First-flash check: let the backlight time out, wait 10 s, press
 * a button — the screen must come back with the right content, not white.
 * ROLLBACK: set this to 0. The LED still times off exactly as before; only the
 * LCD_SLEEP at the BL_OFF edge and the wake block's lcd_wake go away (the
 * wake block is then a no-op, since panel_slept never sets).
 */
/* DEVICE RESULT 2026-09-13: WHITE on the first wake (backlight timed out
 * while playing, ~1 min dark, Menu → solid white). Rolled back to 0 as the
 * note above says. What the bench still needs: whether the white is the
 * first present racing the panel init (then the absorb window is wrong)
 * or LCD_SLEEP itself needing a different wake command on this BCM. */
#ifndef PANEL_SLEEP_AT_IDLE
#define PANEL_SLEEP_AT_IDLE  0
#endif

/*
 * The UI: one event loop that pumps the background player every pass and
 * dispatches input to the current screen (Main menu / Music menu / Browser /
 * Now Playing) on a stack. MENU pops the screen WITHOUT stopping playback, so a
 * song keeps going while you navigate. Never returns.
 */
_Noreturn static void run_ui(fat32_t *fs)
{
    clickwheel_init();
    player_init(fs);
    /* The charger's budget, from the setting (defaults until config_load
     * below, then settings_apply() re-applies the saved choice). Done this
     * early so a device booted on the cable draws the budget from the first
     * second: at the LTC4066's 100 mA default the boot-time library load and
     * spin-ups run off the cell even with the cable in. DEVICE 2026-09-13:
     * on USB the jack carried the drive and the piezo (a cable ground loop)
     * and a whine, and 100 mA was quieter — that is what Charge Rate = 100 mA
     * is for. DEVICE 2026-09-19 (event log): at 100 mA the cell did not
     * climb while the device was used at all. */
    charger_apply();
    artcache_init();                  /* ways must start at key -1; .bss gives 0,
                                       * which is album 0's real index */
    /*
     * Settings: defaults FIRST, then let the saved record overwrite them.
     * config_load() only touches g_settings when it finds a record whose
     * magic, version and CRC all check out, so any failure — file absent,
     * torn write, corrupt slot, unresolvable cluster — leaves the full
     * default set in place. Defaults are the floor, never skipped.
     *
     * Read BEFORE the library (it needs only `fs`, and the drive is spinning
     * right after the mount either way) so the saved theme is known before
     * anything paints a loading screen. Boot Details' OTHER bucket — total
     * minus the named phases — absorbs this read either way; g_lib_load_ms is
     * measured inside library_ensure and is unaffected.
     */
    int clock_mark_unsaved = 0;           /* see the timesync block below */
    settings_defaults(&g_settings);
    int cfg_ok = config_load(fs, &g_settings);
    uart_puts("core: cfg load ");
    uart_put_hex32((uint32_t)cfg_ok);
    uart_puts(" writable ");
    uart_put_hex32((uint32_t)config_writable());
    uart_puts(" seq ");
    uart_put_hex32(config_seq());
    /* The address a save WOULD target, printed before any write can happen.
     * The first on-device bring-up must check this against the LBA
     * tools/make_config.py --verify computes on the host (see the procedure
     * at the top of kernel/config.c) — a mismatch here is the failure mode
     * that destroys a music library. */
    uint32_t cfg_lba0 = 0, cfg_lba1 = 0;
    (void)config_probe_lba(0, &cfg_lba0);
    (void)config_probe_lba(1, &cfg_lba1);
    uart_puts(" lba ");
    uart_put_hex32(cfg_lba0);
    uart_putc('/');
    uart_put_hex32(cfg_lba1);
    uart_putc('\n');
    g_diag_cfg_lba[0] = cfg_lba0;         /* Boot Details, without a spin-up      */
    g_diag_cfg_lba[1] = cfg_lba1;

    /*
     * THE CLOCK, decided once, here, while the drive is still spinning from
     * the mount and before anything else can go wrong.
     *
     * The host cannot tell us the time — on the cable it is Apple's ROM disk
     * mode answering, not us — so `core sync` / `core eject` / `core install`
     * leave a stamp in the record we just loaded, and this is where it is
     * judged. kernel/timesync.c owns the rules (one action per stamp, and a
     * stamp the RTC has already run ten minutes past is inert); main.c owns
     * only the wiring: read the chip, ask, act, record.
     *
     * The mark is FORCE-committed immediately rather than left to the debounce
     * because the boot's own cfg_commit_clear() is a few lines below, and a
     * mark that never reached the platter would let the same stamp be applied
     * again on the next boot.
     */
    {
        /* ONE pass over the chip: the raw bytes are logged and the decision is
         * made from exactly those bytes. Reading twice would cost three more
         * I2C transactions and, worse, let the log line and the decision
         * disagree — and that line is the only evidence the register map is
         * right at all (docs/hw/06-power.md, "Bench procedure"). */
        uint32_t   now_us = mmio_read32(USEC_TIMER_ADDR);
        uint8_t    raw[RTC_REG_COUNT];
        datetime_t rtc_civil;
        uint32_t   rtc_epoch = 0;
        int        rtc_rc    = -1;            /* the bus did not answer */

        if (rtc_read_raw(raw) == 0) {
            uart_puts("core: rtc raw");
            for (int i = 0; i < RTC_REG_COUNT; i++) {
                uart_putc(' ');
                uart_put_hex32(raw[i]);
            }
            rtc_rc = rtc_decode(raw, &rtc_civil);
            if (rtc_rc == 1) {
                rtc_epoch = datetime_to_epoch(&rtc_civil);
            }
            uart_puts(" valid ");
            uart_put_hex32((uint32_t)rtc_rc);
            uart_puts(" epoch ");
            uart_put_hex32(rtc_epoch);
            uart_putc('\n');
        }

        wallclock_anchor(&g_wclock, rtc_rc == 1, rtc_epoch, now_us);
        g_clock_tick_us   = now_us;
        g_clock_resync_us = now_us;
        g_clock_retry     = (rtc_rc < 0);   /* no answer: ask again in a minute */

        timesync_state_t ts = {
            g_settings.host_epoch, g_settings.host_off_min,
            g_settings.applied_epoch, g_settings.utc_off_min,
            timesync_host_dst(g_settings.host_epoch,
                              (unsigned)g_settings.host_dst_raw),
        };
        timesync_action_t act = timesync_decide(&ts, config_writable(),
                                                rtc_rc == 1, rtc_epoch);
        int set_rc = 0;
        if (act == TIMESYNC_SET) {
            set_rc = hal_rtc_set(ts.host_epoch);
            if (set_rc == 0) {
                wallclock_anchor(&g_wclock, 1, ts.host_epoch, now_us);
            } else {
                /* The write did not take. Leave the mark alone so the next
                 * boot retries the same stamp; rule 5 bounds how far back
                 * that can ever pull the clock. */
                act = TIMESYNC_NONE;
            }
        }
        if (timesync_apply(&ts, act)) {
            g_settings.applied_epoch = ts.applied_epoch;
            g_settings.utc_off_min   = ts.utc_off_min;
            settings_touch();
            settings_commit(CFG_COMMIT_FORCE);
            /*
             * THIS WRITE IS PRE-POLICY, deliberately. battery_disk_writes_
             * allowed() answers from `bat_level`, and the policy cannot move
             * that until its median ring holds BATTERY_FILTER_N samples — five
             * of them, 5 s apart, so roughly 20 s into a boot that is still
             * spinning the drive up (hal/hw/battery.c). Sampling the cell here
             * would not change that: five conversions microseconds apart are
             * not a median over time, they are one spin-up-sagged reading with
             * a quorum, which is the decision the filter exists to refuse.
             *
             * That is acceptable for exactly this write: the platters are
             * already up from the mount (the index load follows), it is one sector,
             * and SHUTOFF cannot have fired yet (nothing has been able to
             * judge the cell). Every later save meets the armed gate.
             *
             * What CAN still happen is a refusal for another reason, or a
             * failed write — and either leaves the change PENDING, which
             * cfg_commit_clear() at the end of this boot would throw away with
             * the load's own dirt. Carry the fact forward so the mark is
             * re-armed instead of lost; a lost mark means this stamp is
             * applied again on the next boot, and on the one after that, for
             * as long as the writes keep failing.
             */
            clock_mark_unsaved = g_cfg_commit.dirty;
        }

        uart_puts("core: timesync host ");
        uart_put_hex32(g_settings.host_epoch);
        uart_puts(" off ");
        uart_put_hex32((uint32_t)(int32_t)g_settings.host_off_min);
        uart_puts(" applied ");
        uart_put_hex32(g_settings.applied_epoch);
        uart_puts(" rtc ");
        uart_put_hex32(rtc_epoch);
        uart_puts(act == TIMESYNC_SET   ? " -> set"   :
                  act == TIMESYNC_STALE ? " -> stale" : " -> none");
        uart_puts(" rc ");
        uart_put_hex32((uint32_t)set_rc);
        uart_putc('\n');
    }

    /* The saved theme is known now. Apply it BEFORE the library load so the
     * loading screen (and the album-chip placeholders below) come up in the
     * user's theme; settings_apply() later re-applies it harmlessly along with
     * the audio settings, which want the codec path settled first. */
    theme_set(g_settings.theme);
    boot_screen_render("LOADING", -1);    /* the same screen, now themed          */

    chip_placeholder_init();              /* after theme_set: it bakes
                                           * LINEN_BORDER in at call time
                                           * (settings_apply re-bakes it on a
                                           * live theme change) */
    library_ensure(fs);                   /* preload the index at boot (drive is
                                           * spinning) so Songs/Albums/Artists/
                                           * Genres open INSTANTLY, like Apple —
                                           * not a multi-second stall on first use */
    battery_refresh(1);                   /* prime the status-strip gauge         */
    g_volume = hal_volume_get();          /* reflect the codec's default gain      */
    g_dir_depth = 0;
    g_browse_n  = 0;
    g_br_sel = g_br_accum = 0;
    g_main_sel  = 0;
    g_music_sel = MU_ALBUMS;
    g_menu_accum = 0;
    g_artist_filter[0] = '\0';
    g_artist_sel = g_artist_accum = 0;

    /*
     * The event log: locate CORELOG.BIN, validate its header, find the
     * write cursor. Everything narrated so far this boot is already in the
     * capture ring and goes out in the first block. The line below is the
     * log's own first-flash gate: the two LBAs must equal what
     * tools/make_log.py --verify computed on the host (see kernel/evlog.h
     * and the procedure at the top of kernel/config.c, which this module
     * owes as the write path's second caller). `prev` is the boot reason as
     * far as the log knows it — whether the previous session's last block
     * was a forced (final) flush, or the session just stopped.
     */
    int ev_on = evlog_mount(fs, ata_write_sectors, ata_wakeup);
    uart_puts(ev_on ? "core: evlog on seq " : "core: evlog off seq ");
    uart_put_hex32(evlog_seq());
    uart_puts(" boot ");
    uart_put_hex32(evlog_boot_id());
    uart_puts(evlog_prev_final() < 0 ? " prev none" :
              evlog_prev_final()     ? " prev final" : " prev unflushed");
    uint32_t ev_lba0 = 0, ev_lban = 0;
    (void)evlog_probe_header_lba(&ev_lba0);
    (void)evlog_probe_lba(evlog_seq(), &ev_lban);
    uart_puts(" lba ");
    uart_put_hex32(ev_lba0);
    uart_putc('/');
    uart_put_hex32(ev_lban);
    uart_putc('\n');
    g_diag_log_lba[0] = ev_lba0;          /* Boot Details, without a spin-up      */
    g_diag_log_lba[1] = ev_lban;

    /*
     * The On-The-Go live list: COREOTG.DAT's newest slot into g_otg, then the
     * locator pairs bound to songs (the library is loaded by now). The two
     * LBAs are this module's own first-flash gate and mean exactly what
     * config.c's and evlog.c's do — they MUST equal what
     * tools/make_otg.py --verify computed on the host, checked BEFORE the
     * first add, or the write that follows lands somewhere else on the disk.
     */
    int otg_ok = otg_store_mount(fs, ata_write_sectors, ata_wakeup, &g_otg);
    otg_bind_all();
    uart_puts("core: otg load ");
    uart_dec((int)g_otg.n);
    uart_puts(" writable ");
    uart_put_hex32((uint32_t)otg_store_writable());
    uart_puts(" seq ");
    uart_put_hex32(otg_store_seq());
    uint32_t otg_lba0 = 0, otg_lba1 = 0;
    (void)otg_store_probe_lba(0, &otg_lba0);
    (void)otg_store_probe_lba(1, &otg_lba1);
    uart_puts(" lba ");
    uart_put_hex32(otg_lba0);
    uart_putc('/');
    uart_put_hex32(otg_lba1);
    uart_putc('\n');
    (void)otg_ok;                         /* the count above already says it */

    settings_apply();                     /* push shuffle/repeat/volume out       */
    /* Pick up where the user left off — after settings_apply (it needs the
     * saved volume, which the restore mutes across the open and puts back) and
     * after library_ensure (the locator is resolved against the library). Comes
     * up PAUSED; nothing here starts audio. A no-op when Resume is off, when
     * nothing was saved, or when the saved track can't be resolved. */
    uint32_t resume_t0 = boot_ms_now();
    resume_restore(fs);
    g_boot_resume_ms   = boot_ms_now() - resume_t0;
    /* Everything the user waits for is now done; the next thing that happens is
     * the first UI paint. Stamped here rather than after the present so the
     * number means "time until the device was ready", not "+1 frame". */
    g_boot_total_ms    = boot_ms_now();
    cfg_commit_clear(&g_cfg_commit);      /* loading is not a change to save back */
    otg_store_commit_clear();             /* ...nor is loading the On-The-Go list */
    if (clock_mark_unsaved) {
        /* ...but the clock's mark IS a change, and it did not reach the platter
         * above. Re-arm it for the idle path rather than let the line above
         * discard it (kernel/timesync.h: a stamp acted on but not marked is
         * acted on again next boot). */
        settings_touch();
        uart_puts("core: timesync mark not saved yet — retrying\n");
    }
    g_scr_n = 0;
    scr_push(SCR_MENU);

    int      dirty = 1;
    uint32_t np_last = 0xFFFFFFFFu;
    int      np_first = 1;
    int      np_vol_prev = 0;            /* volume overlay was up last NP paint  */
    int      np_vol_dirty = 0;           /* g_volume moved since the plate paint */
    uint32_t last_present = 0;           /* rate-limit UI presents while playing */
    uint32_t last_bars = 0;              /* rate-limit the now-playing bar anim  */
    uint32_t last_chip = 0;              /* rate-limit album-cover chip loads    */
    uint32_t last_uistat = 0;            /* 5 s cadence of the "core: ui" line   */
    uint32_t last_mq = 0;                /* rate-limit the marquee scroll        */
    int      hold_prev = clickwheel_hold() ? 1 : 0;  /* seed hold-edge detect    */
    int      ext_prev  = power_is_external() ? 1 : 0; /* seed plug-in edge detect */
    int      banner_up = 0;          /* a top-chrome banner is on screen     */
    char     az_prev = 0;                /* A-Z locator letter on screen         */
    int      toast_prev = 0;             /* low-battery toast on screen          */
    int      bat_glyph_prev = battery_glyph_key(g_bat_pct); /* strip gauge as drawn */
    keyhold_t play_key;                  /* PLAY: tap = pause, hold = sleep       */
    keyhold_t menu_key;                  /* MENU: hold = the main menu            */
    keyhold_t row_key;                   /* SELECT on a row: tap acts, hold adds  */
    keyhold_reset(&play_key);            /* (g_ff/g_rw are statics: born idle)    */
    keyhold_reset(&menu_key);
    keyhold_reset(&row_key);
    g_rowsel.pending = 0;
    /* Nothing believed about the jack yet. The first pass primes it from the
     * HAL's own first (already primed) sample, so a boot with an empty jack
     * yields "out" with no edge and no pause. */
    jackwatch_reset(&g_jack);
    g_locked = hold_prev;

    /* Backlight inactivity: full -> dim -> off. Any input wakes to full; a press
     * that wakes from fully-OFF is swallowed (it just lights the screen, the way
     * a real iPod's first touch does). Playback keeps running the whole time. */
    enum { BL_OFF, BL_DIM, BL_FULL };
    int      bl_state   = BL_FULL;
    int      cpu_idled  = 0;              /* core dropped to 30 MHz for deep idle   */
    /* LCD panel put to sleep at backlight-off (PANEL_SLEEP_AT_IDLE). While set,
     * the input sites must NOT light the LED: the "Panel wake" block below
     * wakes the panel, paints, presents the full frame (which retires the
     * BCM's panel init) and only then lights it — see the note above run_ui. */
    int      panel_slept = 0;
    uint32_t panel_refused_at_sleep = 0;  /* lcd_presents_refused() at the sleep */
    uint32_t last_input = mmio_read32(USEC_TIMER_ADDR);
    idlesleep_reset(&g_idlesleep, last_input);   /* two minutes from here, untouched,
                                                  * and a fresh boot sleeps too — a
                                                  * cold boot from a dead cell used
                                                  * to sit awake until dead again */

    /* Seeded from the LIVE transport, not from zero: a successful resume_restore
     * has already left a track loaded, and a `was_active` of 0 would read the
     * first pass as a play->stop edge (closing the codec under a track we just
     * restored) and as a fresh capture of a position we only just loaded. */
    int was_active = player_active();     /* detect the active->idle edge          */
    int last_qidx  = was_active ? player_queue_current() : -1;
    /* Which track is open, for the seek-aim cancel below. Keyed on the OPEN
     * and not on last_qidx because repeat-one re-opens the same queue index. */
    uint32_t last_oseq = player_open_seq();
    g_resume_open_seq   = player_open_seq();
    g_resume_was_paused = player_paused();
    /* Seeded AFTER resume_restore: a restore that had to skip a broken track
     * bumps the end counter, and that must not read as "the queue finished". */
    g_resume_end_seq    = player_end_seq();
    g_resume_last_us    = mmio_read32(USEC_TIMER_ADDR);
    for (;;) {
        /* Time the pump: when it returns in a few microseconds the decode step
         * produced nothing (PCM ring full) and the disk buffer isn't filling —
         * i.e. this pass had no work. Used at the bottom of the loop to halt. */
        uint32_t pump_t0 = mmio_read32(USEC_TIMER_ADDR);
        player_pump();
        uint32_t pump_us = mmio_read32(USEC_TIMER_ADDR) - pump_t0;

        /* TRACK-CHANGE EDGE — unconditional, above every screen-specific branch.
         * Auto-advance moves the queue with no button event, and several screens
         * paint the playing track (the status strip's name on ALL of them, the
         * tracklist's animated bars, the queue view's bars, the main menu's "Now
         * Playing" row). The bar animators are partial painters that only ever
         * repaint the row they find playing, so without a full repaint here the
         * FINISHED track's row keeps a frozen 3-bar glyph forever (one more each
         * time a track ends) and the status strip keeps its name. Covers the
         * play->stop transition too. */
        int now_active = player_active();
        int now_qidx   = now_active ? player_queue_current() : -1;
        if (now_active != was_active || now_qidx != last_qidx) {
            dirty = 1;
        }
        last_qidx = now_qidx;

        /* A RIGHT/LEFT aim belongs to the track it was aimed at, and THIS is
         * the only place that can see that track go. ui/gesture.c is fed a
         * position and a length, and the next track has both, so an aim that
         * survived an auto-advance would go on stepping and then commit a
         * position measured against a track nobody is playing any more — a
         * rewind held through the end of a 4:00 track landing 3:23 into the
         * next one, which the listener has heard none of. Keyed on
         * player_open_seq(), which bumps on every track open (auto-advance,
         * the gapless hand-over, a skip, a queue-view jump and repeat-one's
         * re-open of the SAME index) and not on a seek, so our own committed
         * seek does not look like a track change. The repaint is the dirty
         * above; where it is the queue ENDING, `allowed` drops and the feed
         * would cancel anyway — this just gets there first. */
        uint32_t now_oseq = player_open_seq();
        if (now_oseq != last_oseq || now_active != was_active) {
            last_oseq = now_oseq;
            (void)seekhold_cancel(&g_ff);
            (void)seekhold_cancel(&g_rw);
        }

        /* RESUME POSITION — the edges worth a kilobyte of disk (see the block
         * comment above resume_capture): the track changed, pause flipped,
         * playback started/stopped, or RESUME_SAMPLE_US has gone by. The
         * timestamp is stamped HERE and not inside resume_capture, so a pass
         * where nothing had moved can't re-arm the interval every iteration. */
        {
            uint32_t oseq  = player_open_seq();
            int      paus  = player_paused();
            uint32_t nowu  = mmio_read32(USEC_TIMER_ADDR);
            if (now_active != was_active || oseq != g_resume_open_seq ||
                paus != g_resume_was_paused ||
                (uint32_t)(nowu - g_resume_last_us) >= RESUME_SAMPLE_US) {
                resume_capture();
                g_resume_open_seq   = oseq;
                g_resume_was_paused = paus;
                g_resume_last_us    = nowu;
            }
        }

        /* Queue finished (last track of the album/playlist/song list ended and
         * Repeat is off): no screen may keep showing a dead player. Truncate the
         * stack at the FIRST player view (Now Playing / Queue — the queue view is
         * just as dead), keeping everything the user navigated through below it;
         * an empty stack falls back to the main menu. Testing only the TOP screen
         * left a dead Now Playing behind a Queue view, and left every other screen
         * un-repainted. */
        if (was_active && !now_active) {
            /*
             * If the queue ended BY ITSELF, drop the saved resume position.
             * resume_capture() deliberately keeps the last locator when
             * nothing is loaded ("nothing loaded — keep the last one"), which
             * is right for a stop but wrong here: the listener heard the track
             * to the end, so restoring it on the next boot re-presents a
             * finished song cued at 0:00 (device, 2026-07-27).
             *
             * Gated on player_end_seq() rather than on this transition alone,
             * so a manual stop still keeps its position.
             */
            uint32_t eseq = player_end_seq();
            if (eseq != g_resume_end_seq) {
                g_resume_end_seq = eseq;
                if (resume_ctx_clear(&g_settings)) {
                    settings_touch();  /* locator AND queue context, together */
                }
            }
            /* Playback truly ended (stop / queue exhausted / skip past the ends —
             * NOT an inter-track advance, which never reads inactive here since
             * advance+open complete inside one player_pump). Power the codec down
             * and gate the audio clocks; the next track's hal_audio_init does a
             * full pop-suppressed bring-up, so resume is clean. */
            hal_audio_close();
            for (int i = 0; i < g_scr_n; i++) {
                if (g_scr[i] == SCR_NOWPLAYING || g_scr[i] == SCR_QUEUE) {
                    g_scr_n = i;
                    break;
                }
            }
            if (g_scr_n == 0) {
                g_scr[0]   = SCR_MENU;
                g_scr_n    = 1;
                g_main_sel = 0;
            }
            dirty = 1;
        }
        was_active = now_active;
        if (battery_refresh(0)) {
            /* The strip's gauge is only ever painted on `dirty`, and an idle
             * list screen sets it for nothing, so a menu left alone showed
             * the boot-time gauge indefinitely. Repaint when the glyph as
             * DRAWN changes — at most once per 5 s sample, and not at all
             * for a sample that rounds to the same picture. The charging
             * screen shows the number itself, so every sample is a change. */
            int key = battery_glyph_key(g_bat_pct);
            if (key != bat_glyph_prev || scr_cur() == SCR_CHARGING ||
                (scr_cur() == SCR_SETTINGS && g_set_screen == SETTINGS_BATTERY)) {
                bat_glyph_prev = key;      /* the Battery page IS the sample */
                dirty = 1;
            }
        }

        /*
         * The clock. Three things, all of them cheap, none of them per frame:
         *
         *   - fold the elapsed microseconds into the software clock every 5 s.
         *     The USEC_TIMER wraps every ~71.6 minutes and an unsigned delta
         *     cannot tell one wrap from none (kernel/wallclock.h);
         *   - re-anchor from the chip every half hour — three I2C transactions
         *     an hour, and the only thing that corrects the timer's drift;
         *   - repaint on the MINUTE EDGE, and only where a clock is drawn.
         *     One comparison per pass, and then an ordinary `dirty` — a full
         *     paint and present, once a minute, on a screen that is idle by
         *     definition (a playing device repaints far more often than that).
         *     A band-only present would be the cheaper thing to reach for, but
         *     the clock sits in the header on the main menu and in a list row
         *     under Settings, neither of which is the strip's band.
         */
        {
            uint32_t now_us = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(now_us - g_clock_tick_us) >= CLOCK_TICK_US) {
                wallclock_tick(&g_wclock, now_us);
                g_clock_tick_us = now_us;
            }
            uint32_t due = (g_clock_retry ? RTC_RETRY_S : RTC_RESYNC_S) * 1000000u;
            if ((uint32_t)(now_us - g_clock_resync_us) >= due) {
                clock_resync("resync", 1);
            }
            uint32_t minute = 0;
            if (wallclock_minute(&g_wclock, now_us, &minute) &&
                minute != g_clock_min) {
                g_clock_min = minute;
                /*
                 * Only screens that actually show a clock. Date & Time's first
                 * row shows the time it would edit WHATEVER Time in Title says
                 * — that row is how you read the clock with the setting off —
                 * so it is outside the flag's test; the strip (idle) and the
                 * main menu's header are inside it.
                 */
                int on_dt = (scr_cur() == SCR_SETTINGS &&
                             g_set_screen == SETTINGS_DATETIME);
                int in_title = g_settings.time_in_title &&
                               (!player_active() || scr_cur() == SCR_MENU);
                if (on_dt || in_title) {
                    dirty = 1;
                }
            }
        }

        /*
         * The headphone jack. All of the policy is in ui/jackwatch.c; this is
         * sample, act, narrate.
         *
         * Sitting here, outside the g_locked branch and above the charging
         * modal, is deliberate: a yank in the pocket is the canonical case,
         * and neither Hold nor a modal may swallow it. The pause needs no
         * resume-position call of its own — the capture above fires on any
         * pause flip.
         *
         * The RAW level is read in every build, trusted or not: one 32-bit
         * read of the register the hold switch is already read from, and it
         * is the whole on-screen probe (hal/hw/headphone.h, "WHICH PROBE").
         */
        {
            int raw = headphone_raw();
            if (jackwatch_note_raw(&g_jack, raw)) {
                jack_narrate_raw(raw, g_jack.raw_edges);
                if (scr_cur() == SCR_SETTINGS && g_set_screen == SETTINGS_ABOUT) {
                    dirty = 1;          /* the digit follows the plug */
                }
            }
            int playing = player_active() && !player_paused();
            switch (jackwatch_feed(&g_jack, hal_headphones_present(), playing,
                                   pump_t0)) {
            case JACKWATCH_PAUSE:
                player_pause();
                dirty = 1;              /* the band must read Paused, like a PLAY tap */
                uart_puts("core: jack out, pause\n");
                break;
            case JACKWATCH_OUT:
                uart_puts("core: jack out\n");
                break;
            case JACKWATCH_IN:
                uart_puts("core: jack in\n");
                break;
            case JACKWATCH_NONE:
                break;
            }
        }

        /* Charging screen: pop up on a plug-IN edge (not if already powered at
         * boot, so it won't spuriously appear), auto-dismiss when unplugged. Any
         * button also dismisses it (handled in the input switch below). */
        int ext = power_is_external() ? 1 : 0;
        if (ext && !ext_prev && scr_cur() != SCR_CHARGING) {
            scr_push_modal(SCR_CHARGING);
            dirty = 1;
        } else if (!ext && scr_cur() == SCR_CHARGING) {
            scr_pop();
            dirty = 1;
        }
        ext_prev = ext;

        /* The low-battery modal. Raised once per OK->DISKSAFE crossing and
         * held until a button dismisses it; the charging screen outranks it,
         * because a plugged-in device has already answered the warning. This
         * is idempotent per pass, so it self-heals if anything else truncates
         * the screen stack underneath it. */
        if (battwarn_screen() == BATTWARN_DISKSAFE &&
            scr_cur() != SCR_BATTERY && scr_cur() != SCR_CHARGING) {
            scr_push_modal(SCR_BATTERY);
            dirty = 1;
        } else if (scr_cur() == SCR_BATTERY &&
                   battwarn_screen() != BATTWARN_DISKSAFE) {
            scr_pop();
            dirty = 1;
        }

        /*
         * PLAY press-length arbitration: a tap toggles pause, a hold of
         * PLAY_HOLD_US sleeps the device (suspend: drive spun down, screen
         * dark, but CPU+RAM alive so wake is INSTANT). Holding on to ~5 s
         * escalates to a true PMU power-down. Decided from LIVE button state
         * once per pass, by the same rule SELECT uses on Now Playing: the
         * down-edge decides nothing, the release before the threshold is the
         * tap, the first pass at the threshold is the hold. The pause toggle
         * used to fire on the down-edge EVENT regardless, so hold-to-sleep
         * paused first (and wake did not resume) and hold-while-paused played
         * for two seconds before sleeping. Locked out while the hold switch is
         * on; a press that woke the backlight or dismissed a modal has its tap
         * swallowed below (keyhold_swallow_tap) but can still hold.
         */
        {
            uint32_t nowp = mmio_read32(USEC_TIMER_ADDR);
            int      down = !g_locked &&
                            (clickwheel_buttons() & WHEEL_BTN_PLAY) != 0;
            /* Two things can put the device to sleep from here, and they must
             * not each carry their own copy of the bookkeeping: they set the
             * flag and the origin stamp, and ONE block below does the sleep. */
            int      want_suspend   = 0;
            uint32_t suspend_origin = nowp;

            switch (keyhold_feed(&play_key, down, nowp, PLAY_HOLD_US)) {
            case KEYHOLD_TAP:
                /* On a row that names music, PLAY plays it (play_tap_start);
                 * everywhere else it is the transport toggle, from any screen,
                 * like a real iPod. */
                switch (play_tap_start(fs)) {
                case PLAY_TAP_STARTED:
                    np_first = 1;
                    dirty    = 1;
                    break;
                case PLAY_TAP_CONSUMED:
                    dirty = 1;           /* started nothing, but not a toggle   */
                    break;
                case PLAY_TAP_PASS:
                    if (player_active()) {
                        player_toggle_pause();
                        dirty = 1;
                    }
                    break;
                }
                break;
            case KEYHOLD_HOLD:
                /* Keep timing the escalation to PMU standby from the moment
                 * the finger went down, not from now. */
                want_suspend   = 1;
                suspend_origin = keyhold_down_us(&play_key);
                break;
            case KEYHOLD_NONE:
                break;
            }

            /*
             * The sleep timer, fed from the same stamp. It runs regardless of
             * what the player is doing and regardless of the Hold switch — a
             * locked, pocketed device is the case the feature is FOR — and it
             * is not reset by input: it is a duration, not an idle timeout.
             */
            switch (sleeptimer_feed(&g_sleep, nowp)) {
            case SLEEPTIMER_TICK:
                dirty = 1;                      /* the token's minute changed */
                break;
            case SLEEPTIMER_FIRE:
                uart_puts("core: sleep timer: expired, sleeping\n");
                /* Pause BEFORE suspending rather than teaching suspend_to_ram
                 * a new argument: with the player already paused its
                 * `was_playing` is 0, so the wake comes back PAUSED — you fell
                 * asleep, and the next press is "where was I", not "play" —
                 * and the resume_capture() inside records the paused position.
                 */
                if (player_active() && !player_paused()) {
                    player_pause();
                }
                /* If a PLAY hold reached its threshold on this same pass, its
                 * down-edge is the EARLIER stamp and the one the 5 s
                 * escalation is documented to be timed from; do not push it
                 * forward to now. */
                if (!want_suspend) {
                    suspend_origin = nowp;
                }
                want_suspend = 1;
                break;
            case SLEEPTIMER_NONE:
                break;
            }

            /*
             * IDLE SLEEP (kernel/idlesleep.h). Two minutes with nothing
             * playing and no input, on battery, with no sleep timer running,
             * and the device sleeps the way a PLAY hold sleeps it: the same
             * suspend_to_ram, so the same wake (any button, instant) and the
             * same 30-minute escalation to a PMU standby. This is the rule
             * that was missing on 2026-09-24: paused, parked, dark and cold,
             * the loop still ran — twelve hours of it in one log, 3972 to
             * 3556 mV.
             *
             * `busy` is every reason to stay up: the DAC running
             * (player_playing, NOT player_active — active is true across a
             * pause), external power (nothing to save, and the Battery page
             * is what the bench watches), a sleep timer armed (a suspend
             * would disarm it). Fed every pass so the stamps stay current;
             * the answer is taken only when nothing else is sleeping us.
             * The origin is now: there is no press to time an escalation
             * from, exactly as the sleep timer's FIRE above.
             */
            {
                int idle_fire = idlesleep_feed(&g_idlesleep, nowp, last_input,
                                               player_playing() ||
                                               power_is_external() ||
                                               sleeptimer_armed(&g_sleep));
                if (idle_fire && !want_suspend) {
                    uart_puts("core: idle: nothing playing, no input for ");
                    uart_dec((int)(IDLESLEEP_TIMEOUT_US / 1000000u));
                    uart_puts(" s, sleeping\n");
                    want_suspend   = 1;
                    suspend_origin = nowp;
                }
            }

            if (want_suspend) {
                /* ANY suspend leaves the timer Off — in one place, so a new
                 * sleep site cannot forget one of the two sides (the FIRE
                 * above has already disarmed g_sleep; this is what keeps the
                 * ROW honest, and what disarms a running timer when PLAY is
                 * held instead). */
                sleeptimer_reset(&g_sleep);
                g_settings.sleep_timer_min = 0;

                /* Forget the transient windows too. Both are start-stamp +
                 * elapsed compares against a counter that wraps every ~71.6
                 * min (ui_window_t), and a sleep is the one thing here that
                 * can last that long: a suspend entered under the Hold banner
                 * or the volume plate could otherwise wake to a stale one for
                 * up to its full span. The wake repaints the real screen. */
                g_lock_flash.armed = 0;
                g_vol_show.armed   = 0;

                /* And the presses this loop was timing, for the same reason
                 * and in the same breath. A RIGHT/LEFT hold that the SLEEP
                 * TIMER interrupts mid-aim would otherwise read as a release
                 * on the first pass back — suspend_to_ram does not return
                 * until every button is up and the latch is drained — and
                 * commit a seek aimed before the nap, to a listener who
                 * pressed a button only to light the screen.
                 *
                 * BEFORE the call, not after it: suspend_to_ram paints and
                 * presents the wake frame itself, and np_aim_target() would
                 * find a live aim and draw the pre-nap target over the
                 * transport band for that frame. Nothing between here and the
                 * call reads these. PLAY is the exception that stays where it
                 * is: suspend WAITS on play_key's button, and on the path
                 * where it triggered the sleep its hold has already fired, so
                 * its release is silent by construction. */
                keyhold_reset(&menu_key);
                seekhold_reset(&g_ff);
                seekhold_reset(&g_rw);

                suspend_to_ram(suspend_origin);              /* returns on wake */
                if (g_standby_refused) {
                    /* The escalation (or the 5 s hold) asked the PMU for a
                     * standby and it refused; enter_standby stopped the
                     * player and relit the screen. Do not have the idle
                     * policy ask again every 32 minutes — once is the
                     * diagnosis. The flag itself is consumed below. */
                    idlesleep_off(&g_idlesleep);
                    uart_puts("core: idle sleep: off for this session (standby refused)\n");
                }
                last_input   = mmio_read32(USEC_TIMER_ADDR);
                last_present = last_input;      /* suspend just presented the wake
                                                 * frame: pace the loop's own
                                                 * repaint behind it, or the two
                                                 * back-to-back full frames meet
                                                 * a busy BCM (device log) */
                bl_state   = BL_FULL;           /* backlight restored on resume */
                panel_slept = 0;                /* suspend woke, presented and
                                                 * lit the panel itself — even
                                                 * one this loop had slept at
                                                 * idle first (lcd_sleep is
                                                 * idempotent); don't wake it
                                                 * a second time below */
                cpu_idled  = 0;                 /* suspend's wake cpu_boost()
                                                 * re-established the ONE boost
                                                 * this loop believes it holds.
                                                 * The sleep timer is the first
                                                 * thing that can suspend from
                                                 * the dark, idled state (a
                                                 * PLAY hold cannot: the press
                                                 * relit the screen and
                                                 * re-boosted a pass earlier),
                                                 * and leaving this set would
                                                 * make the idle block below
                                                 * boost a SECOND time on this
                                                 * same pass — g_boost stuck at
                                                 * 2, so the core never drops
                                                 * to 30 MHz at idle again */
                dirty      = 1;                 /* repaint the current screen */
            }
        }

        /*
         * RIGHT / LEFT and MENU, also by press length and also from LIVE
         * state, for the same reason PLAY is: how long a press lasted is the
         * only thing that tells a skip from a seek, or "back" from "home".
         *
         * RIGHT/LEFT only mean transport on the player screens, and only with
         * a track loaded; ui/gesture.c latches that at the down-edge, so the
         * press that JUMPS to Now Playing from a list cannot turn into a seek
         * the moment it lands there. A skip now happens on the release rather
         * than the down-edge — up to half a second later — which is how the
         * original behaves and how PLAY already behaves here.
         *
         * MENU's tap is NOT taken from here: it already fired at the
         * down-edge in the per-screen switch below, which is what keeps every
         * back-one instant. Only the hold is decided here.
         */
        {
            uint32_t nowg = mmio_read32(USEC_TIMER_ADDR);
            uint32_t btn  = g_locked ? 0u : clickwheel_buttons();
            int      on_player = (scr_cur() == SCR_NOWPLAYING ||
                                  scr_cur() == SCR_QUEUE);
            int      seekable  = on_player && player_active();
            /* The machine only reads these where a hold is in flight, which
             * means the button is down; player_elapsed_s() costs a 64-bit
             * divide, so don't pay for it on every idle pass. */
            uint32_t el = 0, tot = 0;
            if (btn & (WHEEL_BTN_RIGHT | WHEEL_BTN_LEFT)) {
                el  = player_elapsed_s();
                tot = player_total_s();
            }

            seekhold_action_t ffa = seekhold_feed(&g_ff,
                                                  (btn & WHEEL_BTN_RIGHT) != 0,
                                                  nowg, seekable, el, tot);
            seekhold_action_t rwa = seekhold_feed(&g_rw,
                                                  (btn & WHEEL_BTN_LEFT) != 0,
                                                  nowg, seekable, el, tot);
            for (int i = 0; i < 2; i++) {
                seekhold_t *sk = i ? &g_rw : &g_ff;
                switch (i ? rwa : ffa) {
                case SEEKHOLD_SKIP:
                    if (i) player_prev(); else player_next();
                    hal_volume_set(g_volume);   /* re-apply over codec re-init */
                    dirty = 1;
                    break;
                case SEEKHOLD_AIM:
                    /* The hold owns the aim: hand the wheel back to volume if
                     * the scrubber had it, and force the transport band (not
                     * `dirty`, which would push the whole frame every tick). */
                    if (np_scrubbing()) scrub_exit();
                    np_last = 0xFFFFFFFFu;
                    break;
                case SEEKHOLD_COMMIT:
                    /* One seek for the whole hold. A refusal needs nothing:
                     * the band repaints the live position either way. */
                    (void)player_seek_to(seekhold_target(sk));
                    np_last = 0xFFFFFFFFu;
                    break;
                case SEEKHOLD_CANCEL:
                    np_last = 0xFFFFFFFFu;      /* live position comes back    */
                    break;
                case SEEKHOLD_NONE:
                    break;
                }
            }

            if (keyhold_feed(&menu_key, (btn & WHEEL_BTN_MENU) != 0, nowg,
                             GESTURE_MENU_HOLD_US) == KEYHOLD_HOLD) {
                if (scr_pop_to_root()) dirty = 1;   /* at the root: a true no-op */
            }
        }

        /* A refused PMU standby (see enter_standby) has already relit and
         * repainted the screen from outside this loop; resync the backlight
         * and idle bookkeeping so the next press is not treated as a wake. */
        if (g_standby_refused) {
            g_standby_refused = 0;
            last_input = mmio_read32(USEC_TIMER_ADDR);
            bl_state   = BL_FULL;
            panel_slept = 0;              /* enter_standby's fallback already
                                           * did wake -> paint -> present ->
                                           * backlight; not a second time */
            dirty      = 1;
        }

        /* Hold-switch edge (a cheap GPIO read, independent of the wheel block
         * which is gated off while held): flash the Hold banner (where the
         * screen has chrome to host it) and toggle the input lock. Playback
         * is untouched. */
        int held = clickwheel_hold() ? 1 : 0;
        if (held != hold_prev) {
            hold_prev = held;
            g_locked  = held;
            keyhold_reset(&play_key);     /* a press under the switch is void */
            keyhold_reset(&menu_key);
            rowsel_drop(&row_key);
            seekhold_reset(&g_ff);
            seekhold_reset(&g_rw);
            /* Force the banner to REPAINT for the new state. Without this, a second
             * edge (e.g. on->off within the 1 s window) leaves banner_up set
             * from the first edge, so the render guard (!banner_up) suppresses
             * the new banner and the unlock one never shows. */
            banner_up = 0;
            if (banner_hosted()) {
                ui_window_arm(&g_lock_flash);
            } else {
                /* A dark modal: no banner. `dirty` below repaints it, which
                 * is how its corner padlock comes and goes. */
                g_lock_flash.armed = 0;
            }
            last_input = mmio_read32(USEC_TIMER_ADDR);   /* wake the backlight    */
            if (bl_state != BL_FULL) {
                /* A slept panel is lit by the "Panel wake" block, AFTER its
                 * first full present has retired the BCM's panel init. */
                if (!panel_slept) backlight_set(g_settings.backlight_bright);
                bl_state = BL_FULL;
                wheel_accel_reset();      /* don't resume a pre-sleep gesture */
            }
            dirty = 1;
        }

        /* Input is sampled on the 100 Hz tick and latched (clickwheel_service),
         * so a tap that lands while this loop is blocked in a disk read isn't
         * lost — we just drain the latch here. */
        wheel_event_t ev;
        int have_ev = clickwheel_get_event(&ev) ? 1 : 0;
        if (have_ev && g_locked) {
            /* Hold is on: the event is swallowed — DRAINED, so it cannot fire
             * as a stale press the moment Hold comes off (it used to sit in
             * the latch untouched). A BUTTON press re-shows the banner, the
             * reference prototype's blockedByHold, lighting the panel like any
             * other press so the refusal is seen. Wheel motion is dropped
             * silently: pocket friction on the wheel is exactly what Hold is
             * for, and it must not keep the backlight awake. On a dark modal
             * the padlock is already in the corner; the press only lights
             * the panel. */
            if (ev.buttons) {
                banner_up = 0;
                if (banner_hosted()) ui_window_arm(&g_lock_flash);
                last_input = mmio_read32(USEC_TIMER_ADDR);
                if (bl_state != BL_FULL) {
                    if (!panel_slept) backlight_set(g_settings.backlight_bright);
                    bl_state = BL_FULL;
                    wheel_accel_reset();
                    dirty = 1;                /* stale panel: the banner's present is full */
                }
            }
            have_ev = 0;
        } else if (have_ev && g_otg_flash.armed && !g_lock_flash.armed &&
                   (ev.buttons || ev.wheel_delta)) {
            /* The On-The-Go banner is confirmation, not a modal: the first
             * real touch ends it and THIS pass falls through to the normal
             * render with the event applied. Same rule as the UNLOCKED
             * banner below, and the reason nothing is ever applied behind it. */
            g_otg_flash.armed = 0;
            banner_up     = 0;
            dirty = 1;
        } else if (have_ev && g_lock_flash.armed && (ev.buttons || ev.wheel_delta)) {
            /* Hold has just come OFF and the unlock banner is still up: it is
             * confirmation, not a modal, so the first touch of the wheel or a
             * button ends it early and THIS pass falls through to the normal
             * render. Real input only: the Hold edge itself arrives through
             * this drain as an event with no buttons and no delta, in the same
             * pass the edge block armed the banner — without the buttons/delta
             * test it dismissed its own banner and the unlock never showed.
             * With the event applied. Without this the banner's block
             * below `continue`s for the rest of its 1 s window, so every scroll
             * and press in it was processed but nothing was drawn. The LOCKED
             * banner is unaffected: input while locked is swallowed above (and
             * a button there re-arms the window), so it never reaches here. */
            g_lock_flash.armed = 0;       /* ui_window_arm's counterpart          */
            banner_up = 0;
            dirty = 1;
        }
        if (have_ev) {
            last_input = mmio_read32(USEC_TIMER_ADDR);
            /* The tick the event is timed from: a row press's length is
             * measured from HERE, not from the first pass the live sampler
             * happens to see the button down on. */
            const uint32_t ev_us = last_input;
            if (bl_state != BL_FULL) {
                int was_off = (bl_state == BL_OFF);
                /* A slept panel is lit by the "Panel wake" block, AFTER its
                 * first full present has retired the BCM's panel init. */
                if (!panel_slept) backlight_set(g_settings.backlight_bright);
                bl_state = BL_FULL;
                wheel_accel_reset();      /* don't resume a pre-sleep gesture */
                dirty = 1;                    /* repaint anything drawn while off */
                if (was_off) {                /* swallow the wake press */
                    /* Each button voids only its OWN arbiter: a MENU or wheel
                     * wake must not claim the next PLAY tap. PLAY keeps the
                     * weaker swallow — holding it from a dark screen is still
                     * how the device is turned off — while MENU and RIGHT/LEFT
                     * are voided outright, because their long actions (home, a
                     * seek) would otherwise ride on a press that was only ever
                     * meant to light the screen. */
                    if (ev.buttons & WHEEL_BTN_PLAY) {
                        keyhold_swallow_tap(&play_key);   /* ...its release too */
                    }
                    if (ev.buttons & WHEEL_BTN_MENU)   keyhold_void(&menu_key);
                    if (ev.buttons & WHEEL_BTN_SELECT) keyhold_void(&row_key);
                    if (ev.buttons & WHEEL_BTN_RIGHT)  seekhold_void(&g_ff);
                    if (ev.buttons & WHEEL_BTN_LEFT)   seekhold_void(&g_rw);
                    ev.buttons = 0;
                    ev.wheel_delta = 0;
                }
            }
            /* Menu click on a button press (one per down-edge; not on the
             * swallowed backlight-wake press above). Wheel clicks are emitted by
             * wheel_move itself, only when the cursor actually advances a row —
             * so a sub-threshold tick that doesn't move can't click. */
            if (ev.buttons) {
                ui_click();
            }
            /* Charging screen: ANY press just dismisses it, and the press is
             * CONSUMED here — above the global transport block, which otherwise
             * saw the same event and paused/skipped the music you were only
             * trying to get the modal off the screen for. Same swallow pattern as
             * the backlight wake above. */
            /* Any input dismisses a live toast (not consumed — the press was
             * meant for whatever is underneath) and marks the modal seen.
             * Not for a swallowed wake press: that one only lit the screen,
             * and it must not also count as "the user has seen the warning". */
            if (ev.buttons || ev.wheel_delta) {
                battwarn_input(mmio_read32(USEC_TIMER_ADDR));
            }
            if ((scr_cur() == SCR_CHARGING || scr_cur() == SCR_BATTERY) &&
                ev.buttons) {
                scr_pop();
                dirty = 1;
                if (ev.buttons & WHEEL_BTN_PLAY) {
                    keyhold_swallow_tap(&play_key);   /* PLAY's release too */
                }
                /* ...and the press that got the modal off the screen must not
                 * also walk the user home or seek, so those are voided whole
                 * (same rule as the backlight wake above). */
                if (ev.buttons & WHEEL_BTN_MENU)   keyhold_void(&menu_key);
                if (ev.buttons & WHEEL_BTN_SELECT) keyhold_void(&row_key);
                if (ev.buttons & WHEEL_BTN_RIGHT)  seekhold_void(&g_ff);
                if (ev.buttons & WHEEL_BTN_LEFT)   seekhold_void(&g_rw);
                ev.buttons     = 0;
                ev.wheel_delta = 0;
            }
            /* RIGHT/LEFT are transport ONLY on the player screens (Now Playing
             * and the queue view, which lists the live queue with the playing
             * row marked), and WHAT the transport is comes from press length
             * in the seekhold block above, not from here — a tap skips, a hold
             * seeks. This drain has two jobs left. On the player screens it
             * hands over a down-edge the live sampler never got to see. On
             * every other screen it owns the press outright: a skip from a
             * list you were merely browsing changed the music under you, so
             * instead RIGHT jumps to Now Playing — PUSHED over the current
             * screen, so MENU from there lands back exactly where you were
             * (inside an album's tracklist, mid-browse) — and LEFT does
             * nothing (MENU is already "back" on every screen; a second back
             * key is a second convention). Nothing here ever runs on a press
             * that woke the backlight, dismissed a modal or landed under Hold
             * — all three zeroed ev.buttons. */
            if (scr_cur() == SCR_NOWPLAYING || scr_cur() == SCR_QUEUE) {
                /* The latch's promise, kept for the transport too. A tap whose
                 * press AND release both fell inside one blocked pass — the
                 * second RIGHT of a double-skip, landing while player_next()
                 * opens a file on a drive the spin-down parked — is never seen
                 * by the 100 Hz live sampler the seek machines read, but the
                 * tick latched its down-edge and it is in this event. Hand it
                 * over; the machine reports it as a skip on the next feed, so
                 * the skip keeps exactly one implementation. A button still
                 * down is handed over too: the machine latches the edge and
                 * judges it at that next feed — still down, it owns the press
                 * as a normal hold; already up, the whole press fell in this
                 * gap and it was a tap. */
                uint32_t live = clickwheel_buttons();
                if (ev.buttons & WHEEL_BTN_RIGHT) {
                    seekhold_missed_tap(&g_ff, (live & WHEEL_BTN_RIGHT) != 0);
                }
                if (ev.buttons & WHEEL_BTN_LEFT) {
                    seekhold_missed_tap(&g_rw, (live & WHEEL_BTN_LEFT) != 0);
                }
            } else if (ev.buttons & WHEEL_BTN_RIGHT) {
                /*
                 * Off the player screens this press is spent on whatever the
                 * drain does with it and is never a transport, so the seek
                 * machine is told so UNCONDITIONALLY, before anything else
                 * decides what the press was for.
                 *
                 * It has to be unconditional because the two things that look
                 * at this press run in a fixed order and can straddle it: the
                 * machine is fed from the LIVE button state at the top of the
                 * pass, and this drain reads the TICK-LATCHED event further
                 * down. A press that begins between those two — any press
                 * during a long pass, a cover read, a spin-up, a present —
                 * was not down when the machine sampled, so it latched
                 * nothing; the drain then pushes Now Playing, and the NEXT
                 * pass shows the machine a fresh down-edge with `allowed`
                 * now 1. Without the void it owns that press outright: a
                 * release under GESTURE_SEEK_HOLD_US skips the track the user
                 * was only trying to look at, and a hold seeks it. The void
                 * is what makes the press dead for the rest of its life.
                 *
                 * It touches only the machine, never ev.buttons, so Search's
                 * picker still gets its RIGHT below: the space bar is the one
                 * place off the player screens where the press means something
                 * to a screen, and a 39-cell ring has nowhere else to put one.
                 * (In PICK the press is safe from the machine either way — it
                 * began on a non-player screen, so the feed latches
                 * allowed_at_down = 0 — but a hold that reaches Now Playing by
                 * some other route must not wake up as a seek.) In RESULTS the
                 * global rule below applies again.
                 */
                seekhold_void(&g_ff);
                if (player_active() &&
                    !(scr_cur() == SCR_SEARCH && g_search.mode == SEARCH_PICK)) {
                    /* Now Playing is never beneath a list today (only MENU or a
                     * SELECT-hold leave it, and modals eat every press), so this
                     * is insurance against a future screen that could sit above
                     * it: a second Now Playing entry would make MENU look dead. */
                    int np_on_stack = 0;
                    for (int i = 0; i < g_scr_n; i++) {
                        if (g_scr[i] == SCR_NOWPLAYING) { np_on_stack = 1; break; }
                    }
                    if (!np_on_stack) {
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                        dirty    = 1;
                    }
                    /* consumed: not a list press */
                    ev.buttons = (uint8_t)(ev.buttons & ~WHEEL_BTN_RIGHT);
                    /* The switch below runs on the NEW top. A thumb landing on
                     * the ring to press RIGHT can latch a sub-detent delta in
                     * the same tick; on Now Playing that reads as a volume
                     * nudge (+ the plate). The press was a jump, nothing else. */
                    ev.wheel_delta = 0;
                }
            }
            switch (scr_cur()) {
            case SCR_MENU:
                if (ev.wheel_delta) {
                    g_main_sel = wheel_move(g_main_sel, main_menu_count(),
                                            ev.wheel_delta, &g_menu_accum);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    if (g_main_sel == MM_MUSIC) {
                        g_music_sel  = MU_ALBUMS;
                        g_menu_accum = 0;
                        scr_push(SCR_MUSIC);
                    } else if (g_main_sel == MM_NOWPLAYING && player_active()) {
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                    } else if (g_main_sel == MM_SETTINGS) {
                        g_set_screen = SETTINGS_ROOT;
                        g_set_sel = g_set_root_sel = g_set_accum = 0;
                        g_set_editing = 0;
                        scr_push(SCR_SETTINGS);
                    } else if (g_main_sel == MM_PLAYLISTS) {
                        playlists_ensure(fs);          /* same list as Music -> */
                        scr_push(SCR_PLAYLISTS);       /* Playlists; MENU pops here */
                    }
                    /* other items are greyed: SELECT does nothing yet */
                    dirty = 1;
                }
                break;

            case SCR_MUSIC:
                if (ev.wheel_delta) {
                    g_music_sel = wheel_move(g_music_sel, MU_COUNT,
                                             ev.wheel_delta, &g_menu_accum);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    if (g_music_sel == MU_ALBUMS) {
                        library_ensure(fs);            /* index -> g_albums      */
                        g_dir_depth = 0;
                        g_artist_filter[0] = '\0';     /* all albums             */
                        albumview_build(0);
                        albumlist_queue_chips();       /* start loading covers   */
                        g_br_sel = g_br_accum = 0;
                        g_br_from_search = 0;
                        scr_push(SCR_BROWSER);
                    } else if (g_music_sel == MU_ARTISTS) {
                        library_ensure(fs);            /* index -> g_albums      */
                        g_artist_filter[0] = '\0';
                        /* No rebuild: library_ensure built the list when the
                         * index loaded, and nothing changes g_albums after. */
                        g_artist_sel = g_artist_accum = 0;
                        scr_push(SCR_ARTISTS);
                    } else if (g_music_sel == MU_SONGS) {
                        library_ensure(fs);
                        songview_build(-1, 0);         /* all songs, all artists */
                        scr_push(SCR_SONGS);
                    } else if (g_music_sel == MU_SHUFFLE) {
                        shuffle_songs_play(fs);        /* the library, shuffled */
                        hal_volume_set(g_volume);      /* re-apply over codec re-init */
                        if (player_active()) {
                            scr_push(SCR_NOWPLAYING);
                            np_first = 1;
                        }
                    } else if (g_music_sel == MU_GENRES) {
                        library_ensure(fs);
                        g_genre_sel = g_genre_accum = 0;
                        scr_push(SCR_GENRES);
                    } else if (g_music_sel == MU_PLAYLISTS) {
                        playlists_ensure(fs);          /* Music/Playlists/NAME.m3u8 */
                        scr_push(SCR_PLAYLISTS);
                    } else if (g_music_sel == MU_SEARCH) {
                        library_ensure(fs);
                        /* Artists are built by library_ensure. The playlist
                         * folder read is once a session — the disk cannot
                         * change under a running firmware — so only the
                         * first entry here or into Playlists reads it. */
                        if (!g_playlists_scanned) playlists_load(fs);
                        /* The query survives leaving the screen, so re-scan
                         * on the way back in: the hits then always name rows
                         * of the lists as they are now. */
                        if (g_search.qlen > 0) search_rescan();
                        scr_push(SCR_SEARCH);
                    }
                    /* other items are greyed: SELECT does nothing yet */
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to main menu */
                    g_menu_accum = 0;
                    dirty = 1;
                }
                break;

            case SCR_ARTISTS:
                if (ev.wheel_delta && g_artists_n > 0) {
                    g_artist_sel = wheel_move(g_artist_sel, g_artists_n,
                                              ev.wheel_delta, &g_artist_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_artists_n > 0) {
                    /* Filter the album list to the chosen artist's folders. */
                    int k = 0;
                    for (; g_artists[g_artist_sel].name[k] && k < NAME_MAX; k++) {
                        g_artist_filter[k] = g_artists[g_artist_sel].name[k];
                    }
                    g_artist_filter[k] = '\0';
                    g_dir_depth = 0;
                    albumview_build(g_artist_filter);
                    albumlist_queue_chips();
                    g_br_sel = g_br_accum = 0;
                    g_br_from_search = 0;
                    scr_push(SCR_BROWSER);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Music menu */
                    dirty = 1;
                }
                break;

            case SCR_SONGS:
                if (ev.wheel_delta && g_songview_n > 0) {
                    g_song_sel = wheel_move(g_song_sel, g_songview_n,
                                            ev.wheel_delta, &g_song_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_songview_n > 0) {
                    /* Tap plays, hold adds to On-The-Go: the press length
                     * decides, in the block beside Now Playing's. */
                    rowsel_arm(g_song_sel, &row_key, ev_us);
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back (Music or Genres) */
                    dirty = 1;
                }
                break;

            case SCR_GENRES:
                if (ev.wheel_delta && g_genres_n > 0) {
                    g_genre_sel = wheel_move(g_genre_sel, g_genres_n,
                                             ev.wheel_delta, &g_genre_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_genres_n > 0) {
                    songview_build(g_genre_sel, 0);     /* this genre's songs */
                    scr_push(SCR_SONGS);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Music menu */
                    dirty = 1;
                }
                break;

            case SCR_PLAYLISTS:
                if (ev.wheel_delta) {
                    g_pl_sel = wheel_move(g_pl_sel, playlists_row_count(),
                                          ev.wheel_delta, &g_pl_accum);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    /* Row 0 is the pinned On-The-Go list; the rest are the
                     * files. No hold action here, so SELECT still acts on the
                     * down-edge — a delayed tap would be latency for nothing. */
                    if (g_pl_sel == 0) {
                        g_otg_free_slot = otg_slot_free_index(fs);
                        g_otg_sel = (g_otg.n > 0) ? OTG_ROW_FIRST : OTG_ROW_CLEAR;
                        g_otg_accum = 0;
                        g_otg_confirm.armed = 0;
                        scr_push(SCR_OTG);
                    } else {
                        playlist_open(fs, g_pl_sel - 1); /* parse + resolve + bind */
                        scr_push(SCR_PLAYLIST);
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back (Music or main) */
                    dirty = 1;
                }
                break;

            case SCR_PLAYLIST:
                if (ev.wheel_delta && playlist_row_count() > 0) {
                    g_plt_sel = wheel_move(g_plt_sel, playlist_row_count(),
                                           ev.wheel_delta, &g_plt_accum);
                    g_pl_confirm.armed = 0;     /* moving off the row cancels it */
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && playlist_row_count() > 0) {
                    /* Tap plays (or deletes a saved slot); hold adds the track
                     * to On-The-Go. */
                    rowsel_arm(g_plt_sel, &row_key, ev_us);
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Playlists */
                    dirty = 1;
                }
                break;

            case SCR_OTG:
                /* Both action rows are greyed and do nothing on an empty
                 * list, so there is nothing to move to and nothing to press:
                 * the cursor stays on row 0 and the wheel is silent. */
                if (ev.wheel_delta && g_otg.n > 0) {
                    g_otg_sel = wheel_move(g_otg_sel, otg_row_count(),
                                           ev.wheel_delta, &g_otg_accum);
                    g_otg_confirm.armed = 0;    /* moving off the row cancels it */
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_otg.n > 0) {
                    /* Tap plays from the row (or clears / saves); hold removes
                     * the row. */
                    rowsel_arm(g_otg_sel, &row_key, ev_us);
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Playlists */
                    dirty = 1;
                }
                break;

            case SCR_SEARCH: {
                if (ev.wheel_delta) {
                    if (g_search.mode == SEARCH_PICK) {
                        /* One cell a detent, wrapping; the click is ours
                         * because the ring is not a list and does not go
                         * through wheel_move. */
                        if (search_ring_move(&g_search, ev.wheel_delta)) {
                            dirty = 1;
                        }
                    } else if (g_search.nhit > 0) {
                        g_search.sel = wheel_move(g_search.sel, g_search.nhit,
                                                  ev.wheel_delta, &g_search.accum);
                        dirty = 1;
                    }
                }
                int act = SEARCH_ACT_NONE;
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    /* A hit is a row: the tap opens it, and a hold on a SONG
                     * hit adds it to On-The-Go like any other song row. The
                     * ring is not a list — its SELECT types a character — so
                     * search_hold_rows() reports no rows there and the press
                     * keeps the down-edge, as a key must. */
                    if (search_hold_rows(&g_search, 0, 0) > 0) {
                        rowsel_arm(g_search.sel, &row_key, ev_us);
                    } else {
                        act = search_key(&g_search, SEARCH_KEY_SELECT);
                    }
                } else if (ev.buttons & WHEEL_BTN_MENU) {
                    act = search_key(&g_search, SEARCH_KEY_MENU);
                } else if (ev.buttons & WHEEL_BTN_RIGHT) {
                    act = search_key(&g_search, SEARCH_KEY_RIGHT);
                } else if (ev.buttons & WHEEL_BTN_LEFT) {
                    act = search_key(&g_search, SEARCH_KEY_LEFT);
                }
                switch (act) {
                case SEARCH_ACT_RESCAN:
                    search_rescan();
                    dirty = 1;
                    break;
                case SEARCH_ACT_TO_RESULTS:
                case SEARCH_ACT_TO_PICK:
                    dirty = 1;
                    break;
                case SEARCH_ACT_POP:
                    scr_pop();                          /* back to Music menu */
                    dirty = 1;
                    break;
                case SEARCH_ACT_OPEN:
                    if (search_open_hit(fs)) np_first = 1;
                    dirty = 1;
                    break;
                default:
                    break;
                }
                break;
            }

            case SCR_BROWSER: {
                /* Depth 0 scrolls the album list (g_br_sel); depth 1 the loaded
                 * tracklist (g_det_sel) — separate so backing out restores the
                 * album-list position. */
                int count = (g_dir_depth == 0) ? albumlist_count() : g_browse_n;
                int *sel  = (g_dir_depth == 0) ? &g_br_sel   : &g_det_sel;
                int *acc  = (g_dir_depth == 0) ? &g_br_accum : &g_det_accum;
                if (ev.wheel_delta && count > 0) {
                    *sel = wheel_move(*sel, count, ev.wheel_delta, acc);
                    dirty = 1;                    /* window derived at paint time  */
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && count > 0) {
                    /* Tap enters the album (or plays the track); hold adds the
                     * whole album, or that one track, to On-The-Go. */
                    rowsel_arm(*sel, &row_key, ev_us);
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_dir_depth > 0 && g_br_from_search) {
                        /* Opened straight from a Search hit: back is the
                         * results, not an album list that was never shown.
                         * The depth goes with the screen, as it does in
                         * scr_pop_to_root — this is the one exit from depth 1
                         * that leaves no BROWSER on the stack to own it. */
                        g_br_from_search = 0;
                        g_dir_depth      = 0;
                        scr_pop();
                    } else if (g_dir_depth > 0) {       /* tracklist -> album list */
                        g_dir_depth = 0;
                        albumlist_queue_chips();        /* g_br_sel kept (the album) */
                    } else {                            /* album list -> Music menu */
                        scr_pop();
                    }
                    dirty = 1;
                }
                break;
            }

            case SCR_NOWPLAYING:
                /* Scrubbing: the wheel aims at a position instead of setting
                 * volume. Accelerates with the spin so a 70-minute track is
                 * crossable, but one deliberate detent is always SCRUB_STEP_S. */
                if (ev.wheel_delta && np_scrubbing() && player_active()) {
                    int vel   = wheel_accel_step(ev.wheel_delta);
                    int units = ev.wheel_delta / CW_WHEEL_SENSITIVITY;
                    if (units == 0) {
                        units = (ev.wheel_delta > 0) ? 1 : -1;
                    }
                    int32_t  step  = (int32_t)units * SCRUB_STEP_S * vel;
                    int32_t  t     = (int32_t)g_scrub_target_s + step;
                    uint32_t total = player_total_s();
                    if (t < 0) t = 0;
                    if (total > 0 && t > (int32_t)total) t = (int32_t)total;
                    g_scrub_target_s = (uint32_t)t;
                    g_scrub_last_us  = mmio_read32(USEC_TIMER_ADDR);
                    g_scrub_dirty    = 1;
                    /* Only the transport band shows the target: force ITS
                     * repaint (the clock-tick path) — not `dirty`, which
                     * would re-render and push the whole frame. */
                    np_last          = 0xFFFFFFFFu;
                } else if (ev.wheel_delta) {             /* wheel = volume        */
                    /* The wheel reports raw position units (~CW_WHEEL_SENSITIVITY
                     * per detent), which is why volume used to leap ~10 at a time.
                     * Quantise to whole detents: a single slow detent nudges the
                     * volume by exactly ±1, and spinning faster (more detents per
                     * event) accelerates so you can still sweep the whole range. */
                    /* The rule now lives in ui/wheel.c (wheel_value_step) and
                     * every value wheel shares it: 1 detent = 1, faster
                     * sweeps (2 -> 4, 3 -> 6), capped at 12. */
                    int step = wheel_value_step(ev.wheel_delta,
                                                WHEEL_VALUE_STEP_MAX);
                    /* The Volume Limit is applied by the same helper the
                     * Settings slider uses — one rule, two wheels. No click
                     * either way: this is a slider, not navigation. */
                    int nv = settings_volume_clamp(&g_settings, g_volume + step);
                    if (nv != g_volume) {
                        g_volume = nv;
                        hal_volume_set(g_volume);
                        g_settings.volume = g_volume;     /* keep Settings in sync */
                        settings_touch(); /* debounced: one write per volume sweep */
                    }
                    /* Armed even when the wheel could not move: at a rail —
                     * and especially AT THE LIMIT, where the plate's triangle
                     * is the explanation — the user still needs to see why. A
                     * pinned wheel no longer earns a disk write, though. */
                    ui_window_arm(&g_vol_show);
                    /* The plate is its own present (see the Now Playing
                     * branch of the render step): not `dirty`, which would
                     * push all 38400 words for a 3200-word rect. */
                    np_vol_dirty = 1;
                }
                /* SELECT is now press-length sensitive here: a tap toggles the
                 * scrubber (what SELECT does on Now Playing on a real iPod, and
                 * far and away the more frequent action), a hold opens the queue
                 * view. Only the down-edge is recorded — the decision is made in
                 * the per-pass block below, once the press length is known. */
                if ((ev.buttons & WHEEL_BTN_SELECT) && player_active()) {
                    g_sel_down_us = mmio_read32(USEC_TIMER_ADDR);
                    g_sel_pending = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back, keep playing */
                    dirty = 1;
                }
                break;

            case SCR_QUEUE:
                if (ev.wheel_delta && player_queue_len() > 0) {
                    g_queue_sel = wheel_move(g_queue_sel, player_queue_len(),
                                             ev.wheel_delta, &g_queue_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && player_queue_len() > 0) {
                    if (!player_queue_is_dir(g_queue_sel)) {
                        player_jump(g_queue_sel);          /* play the chosen track */
                        hal_volume_set(g_volume);          /* re-apply over re-init */
                        scr_pop();                          /* back to Now Playing  */
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();
                    dirty = 1;
                }
                break;

            case SCR_SETTINGS: {
                /*
                 * The Date & Time EDITOR is not a list: the wheel moves a
                 * field's value, SELECT confirms a field and the last one
                 * commits, MENU writes nothing at all. ui/settime.c owns every
                 * one of those decisions (and every calendar edge case in
                 * them); this arm is the wiring.
                 */
                if (g_set_screen == SETTINGS_SETTIME) {
                    if (ev.wheel_delta) {
                        /* Detents, not raw ticks: one slow detent is one
                         * minute / hour / day (it was ~4). */
                        settime_adjust(&g_settime,
                                       wheel_value_step(ev.wheel_delta,
                                                        WHEEL_VALUE_STEP_MAX));
                        dirty = 1;
                    }
                    if (ev.buttons & WHEEL_BTN_SELECT) {
                        if (settime_next(&g_settime)) {
                            /* The last field: write the clock. The editor
                             * shows LOCAL time and the chip holds UTC, so the
                             * device's stored offset comes back off here. */
                            datetime_t civil;
                            settime_civil(&g_settime, &civil);
                            /* The editor shows WALL time; the device keeps
                             * standard (Daylight Saving, kernel/datetime.h). */
                            uint32_t local = clock_dst_standard_from_wall(
                                                 datetime_to_epoch(&civil),
                                                 g_settings.dst_on);
                            uint32_t utc   = 0;
                            /* The editor clamps the YEAR to 2001..2099; the
                             * zone can still push the UTC epoch off either end
                             * (2001-01-01 00:30 at UTC+02:00 is 2000 in UTC).
                             * datetime_utc_from_local refuses that rather than
                             * wrapping, and the refusal takes the same path as
                             * a failed write: the old time stands and the log
                             * says so. */
                            int rc = datetime_utc_from_local(
                                         local, g_settings.utc_off_min, &utc)
                                     ? hal_rtc_set(utc) : -3;
                            uart_puts("core: rtc set ");
                            uart_put_hex32(utc);
                            uart_puts(" rc ");
                            uart_put_hex32((uint32_t)rc);
                            uart_putc('\n');
                            if (rc == 0) {
                                wallclock_anchor(&g_wclock, 1, utc,
                                                 mmio_read32(USEC_TIMER_ADDR));
                                g_clock_tick_us   = mmio_read32(USEC_TIMER_ADDR);
                                g_clock_resync_us = g_clock_tick_us;
                                g_clock_min       = 0;   /* repaint the strip */
                            }
                            /* A manual set NEVER touches applied_epoch: a
                             * later host stamp is still news (timesync.h). */
                            g_set_screen = SETTINGS_DATETIME;
                            g_set_sel = g_set_accum = 0;
                        }
                        dirty = 1;
                    }
                    if (ev.buttons & WHEEL_BTN_MENU) {
                        g_set_screen = SETTINGS_DATETIME;   /* nothing written */
                        g_set_sel = g_set_accum = 0;
                        dirty = 1;
                    }
                    break;
                }

                int scount = settings_count(g_set_screen);
                int slider = (settings_kind(g_set_screen, g_set_sel)
                              == SETTINGS_KIND_SLIDER);
                /* NB `slider` is re-read before the SELECT test below: one latched
                 * event can carry BOTH a wheel move and a button, and the move
                 * changes which row SELECT lands on. */
                if (ev.wheel_delta) {
                    if (g_set_editing && slider) {         /* adjust the value  */
                        /* Detents, not raw ticks: this was the raw delta
                         * clamped to +/-4, so ONE slow detent (~4 ticks)
                         * moved every slider by four and no single value
                         * was reachable. The volume wheel's rule now. */
                        int dd = wheel_value_step(ev.wheel_delta,
                                                  WHEEL_VALUE_STEP_MAX);
                        /* Only a value that MOVED earns an apply and a disk
                         * write: a wheel pinned at a rail (volume 100,
                         * brightness 32) is not a change — settings_adjust
                         * says so, and a48510f made SELECT honour the same
                         * answer. */
                        if (settings_adjust(g_set_screen, &g_settings, g_set_sel, dd)) {
                            settings_apply();              /* live volume/etc.  */
                            settings_touch();              /* debounced save    */
                            /* Brightness slider: light up to the new level as
                             * you turn, so the wheel drives the panel in real
                             * time. */
                            if (g_set_screen == SETTINGS_DISPLAY && g_set_sel == 1) {
                                if (!panel_slept) {   /* the Panel-wake block
                                                       * lights it after the
                                                       * first present, else
                                                       * a white flash */
                                    backlight_set(g_settings.backlight_bright);
                                }
                                bl_state = BL_FULL;
                            }
                        }
                    } else if (scount > 0) {               /* move selection    */
                        g_set_sel = wheel_move(g_set_sel, scount,
                                               ev.wheel_delta, &g_set_accum);
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    slider = (settings_kind(g_set_screen, g_set_sel)
                              == SETTINGS_KIND_SLIDER);    /* the CURRENT row   */
                    if (slider) {
                        /* A locked slider (Bass/Treble under an EQ preset) is
                         * a readout: the click happens, edit mode does not. */
                        if (!settings_row_locked(g_set_screen, &g_settings,
                                                 g_set_sel)) {
                            g_set_editing = !g_set_editing; /* enter/exit edit  */
                        }
                    } else {
                        int act = settings_activate(g_set_screen, &g_settings,
                                                    g_set_sel);
                        int target = -1;
                        switch (act) {
                        case SETTINGS_ENTER_PLAYBACK: target = SETTINGS_PLAYBACK; break;
                        case SETTINGS_ENTER_SOUND:    target = SETTINGS_SOUND;    break;
                        case SETTINGS_ENTER_DISPLAY:  target = SETTINGS_DISPLAY;  break;
                        case SETTINGS_ENTER_ABOUT:    target = SETTINGS_ABOUT;    break;
                        case SETTINGS_ENTER_THEME:    target = SETTINGS_THEME;    break;
                        case SETTINGS_ENTER_CLICKER:  target = SETTINGS_CLICKER;  break;
                        case SETTINGS_ENTER_DIAG:     target = SETTINGS_DIAG;     break;
                        case SETTINGS_ENTER_BATTERY:  target = SETTINGS_BATTERY;  break;
                        case SETTINGS_ENTER_DATETIME: target = SETTINGS_DATETIME; break;
                        case SETTINGS_ENTER_SETTIME:
                            /* Seed the editor from the best time we have —
                             * the running clock, else the host's stamp, else
                             * ui/settime.c's own fixed default — and enter it
                             * WITHOUT touching g_set_root_sel: the root's
                             * remembered row belongs to Date & Time, which is
                             * where MENU comes back to. */
                            {
                                datetime_t seed;
                                int have = clock_local_now(&seed);
                                if (!have && g_settings.host_epoch != 0) {
                                    have = datetime_local(g_settings.host_epoch,
                                                          g_settings.host_off_min,
                                                          &seed);
                                }
                                settime_begin(&g_settime, have, &seed,
                                              !g_settings.time_24h);
                            }
                            g_set_screen = SETTINGS_SETTIME;
                            g_set_sel = g_set_accum = 0;
                            g_set_editing = 0;
                            break;
                        default: break;
                        }
                        if (target >= 0) {                 /* descend a screen  */
                            g_set_root_sel = g_set_sel;
                            g_set_screen = target;
                            /* A picker opens on the value in force. */
                            g_set_sel = settings_initial_sel(target, &g_settings);
                            g_set_accum = 0;
                            g_set_editing = 0;
                        } else if (act == SETTINGS_ACTION_DISKMODE) {
                            /* Reboot into the ROM's USB mass-storage mode. We
                             * implement no USB ourselves, so this is the only
                             * way onto a host from inside our own firmware —
                             * the alternative is catching the ROM's Select+Play
                             * window at boot, which is timing-sensitive and easy
                             * to miss. Stop playback and blank the panel first
                             * so we never reset mid-DMA or leave a half-drawn
                             * screen; power_enter_disk_mode never returns. */
                            player_stop();
                            hal_audio_close();
                            /* Disk mode is where the log gets READ, so
                             * what is still in RAM has to land first —
                             * the reboot below is a ROM entry, nothing
                             * of ours runs again. Same for a settings
                             * change made a second ago. Both forced;
                             * the drive is up (the ROM needs it anyway).
                             * DEVICE 2026-09-13: a whole test session
                             * arrived at the host as "prev unflushed". */
                            settings_commit(1);
                            otg_commit(CFG_COMMIT_FORCE);
                            uart_puts("core: disk mode: entering\n");
                            evlog_commit(CFG_COMMIT_FORCE);
                            console_clear(LINEN_SURFACE);
                            ui_text_centered(LCD_HEIGHT / 2 - 8, "Disk Mode",
                                             FONT_TITLE, LINEN_INK);
                            lcd_present_fb(console_framebuffer());
                            power_enter_disk_mode();
                        } else if (act == SETTINGS_ACTION_RESET) {
                            /* THE CLOCK IS NOT A SETTING. settings_defaults()
                             * zeroes all six of its fields (it is also the
                             * pre-load state, where a non-zero stamp would be
                             * one nobody wrote), so the reset puts back the
                             * three that are facts rather than preferences:
                             * the host's stamp and offset, which the device may
                             * not have acted on yet, and the DISPLAY OFFSET,
                             * without which the clock on the strip, in the
                             * header and in the editor would read UTC for the
                             * rest of the session — and a manual set made in
                             * that window would write local-as-UTC to the chip
                             * and jump by the zone at the next boot.
                             *
                             * The MARK is deliberately not restored: the next
                             * boot then re-examines the stamp (rule 5 decides
                             * whether it is still worth applying), which is
                             * what makes "a Reset does not cost you the clock"
                             * true rather than a hope. What DOES reset are the
                             * two rows: 12-hour, no clock in the title. */
                            uint32_t host_epoch = g_settings.host_epoch;
                            int      host_off   = g_settings.host_off_min;
                            int      disp_off   = g_settings.utc_off_min;
                            int      host_dst   = g_settings.host_dst_raw;
                            settings_defaults(&g_settings);
                            g_settings.host_epoch   = host_epoch;
                            g_settings.host_off_min = host_off;
                            g_settings.utc_off_min  = disp_off;
                            g_settings.host_dst_raw = host_dst;
                            settings_apply();
                            sleep_timer_apply();           /* defaults() zeroed
                                                            * the row: disarm  */
                            settings_touch();              /* persist the reset */
                        } else if (act == SETTINGS_ACTION_SLEEPTIMER) {
                            /* The record changed, but only its RUNTIME part:
                             * re-arm the countdown and DO NOT touch. Nothing
                             * on disk stores the duration (ui/settings.h), so
                             * a touch here would spin the drive up three
                             * seconds later to write a byte-identical record —
                             * and it would do it at bedtime, which is the one
                             * moment the drive is parked for the night. */
                            sleep_timer_apply();
                        } else if (act == SETTINGS_ACTION_NONE) {
                            /* Only a row that actually CHANGED the record gets
                             * persisted. SELECT on About/Diagnostics, or on the
                             * theme already selected, used to mark the config
                             * dirty and spin the drive up three seconds later to
                             * write a byte-identical record. */
                            settings_apply();
                            /* Shuffle changed under a playing queue means the
                             * player just dealt a NEW order; the record still
                             * carries the old (seed, keep) until the next
                             * capture edge (a track change, a pause, five
                             * minutes), and a boot inside that window would
                             * deal a valid but different order. Capture now.
                             * resume_capture() is idempotent — it touches only
                             * when something actually moved — so the rows that
                             * are not about playback pay nothing. */
                            resume_capture();
                            settings_touch();
                        }
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_set_editing) {
                        g_set_editing = 0;                 /* exit edit, stay   */
                    } else if (g_set_screen != SETTINGS_ROOT) {
                        g_set_screen = SETTINGS_ROOT;      /* back to root      */
                        g_set_sel = g_set_root_sel;
                        g_set_accum = 0;
                    } else {
                        scr_pop();                          /* leave Settings    */
                        /* The natural "I'm done" moment: commit now rather than
                         * waiting out the debounce, so the record is on the
                         * platter before the user can reach for the Hold switch.
                         * Same exit the MENU hold takes (scr_pop_to_root). */
                        settings_leave();
                    }
                    dirty = 1;
                }
                break;
            }

            case SCR_BATTERY:
            case SCR_CHARGING:
                break;                        /* dismissal is handled above */
            }
        }

        /* Settings > Theme's live preview, or the saved theme back once the
         * picker is left without SELECT. A swap repaints the whole panel. */
        if (theme_preview_sync()) dirty = 1;

        /* Idle-timeout backlight: dim, then off — timeout from Settings (0 =
         * never), brightness from Settings. Playback keeps running. */
        uint32_t dim_us, off_us;
        if (g_settings.backlight_secs <= 0) {
            dim_us = off_us = 0xFFFFFFFFu;      /* never dim/off               */
        } else {
            dim_us = (uint32_t)g_settings.backlight_secs * 1000000u;
            off_us = dim_us + 15u * 1000000u;
        }
        int dim_level = g_settings.backlight_bright / 4;
        if (dim_level < 1) dim_level = 1;
        uint32_t idle = mmio_read32(USEC_TIMER_ADDR) - last_input;
        if (bl_state == BL_FULL && idle > dim_us) {
            backlight_set(dim_level);
            bl_state = BL_DIM;
        } else if (bl_state == BL_DIM && idle > off_us) {
            backlight_set(0);
            bl_state = BL_OFF;
            if (PANEL_SLEEP_AT_IDLE) {    /* see the note above run_ui() */
                /* LED first (above), then the panel: the sleep drains any
                 * in-flight update, drops the panel-enable bits and issues
                 * LCD_SLEEP, ~20 ms of which is a settle the loop sits out
                 * (the pump is not called, but the PCM ring is far deeper
                 * than that). From here every present is refused until the
                 * "Panel wake" block — and nothing here tries: every painter
                 * is gated on bl_state != BL_OFF. */
                lcd_sleep();              /* blank the panel too, not just the LED */
                panel_slept = 1;
                panel_refused_at_sleep = lcd_presents_refused();
            }
        }

        /* Idle CPU-clock scaling: once the screen has timed fully off AND nothing
         * is playing, drop the core 80->30 MHz — only the disk/decode path needs
         * 80. Restore the instant there's life again: any wake back to BL_FULL, or
         * playback starting. Rides the same refcounted boost the boot path took, so
         * it stays balanced (idle unboost 1->0, wake boost 0->1) and re-boosts
         * before the render/decode work later in this same iteration.
         *
         * "Playing" means the DAC is running — player_playing(), NOT
         * player_active(). Active stays set across a pause (the UI needs it to,
         * every transport control is gated on it), so keying this on active
         * meant a PAUSED device with the screen off held 80 MHz indefinitely:
         * the single largest idle-power miss in the firmware, since pause-and-
         * pocket is the common way to stop listening. A paused pump does no
         * decode and feeds no DMA, so 30 MHz is plenty; the resume itself
         * always arrives through a wake press that has already re-lit the
         * screen (and so re-boosted here) one pass earlier. */
        if (cpu_idled && (bl_state != BL_OFF || player_playing())) {
            cpu_boost();
            cpu_idled = 0;
        } else if (!cpu_idled && bl_state == BL_OFF && !player_playing()) {
            cpu_unboost();
            cpu_idled = 1;
        }

        /* Disk spin-down at idle: once nothing needs the drive AND the user has
         * been idle a while, park the platters — the single largest continuous
         * draw on a spinning-HDD unit. "Nothing needs it" = not actively playing;
         * PAUSED counts as idle: the player's paused pump stocks the anti-skip
         * buffer to its low mark while the platters are still up (this park
         * comes 20 s later), so a resume plays from RAM and the drive spins
         * up lazily on the next burst. When the buffer is shallow anyway
         * (paused and parked a second into a track), player_resume pre-pays
         * the spin-up before the DAC starts. DEVICE 2026-09-13: before that,
         * a track restored paused at boot resumed into a synchronous read
         * on this parked drive — 11 underruns. Skipped while actively
         * playing: player.c already parks the drive between bursts and owns that
         * cadence. ata_is_parked() is the shared truth, so we never re-issue
         * STANDBY on an already-parked drive. On an iFlash/SSD mod this is a
         * harmless no-op. */
        /* Debounced settings write. Deliberately placed BEFORE the spin-down
         * check below: if a save is due we want it to land while the platters
         * are still turning, rather than parking the drive and immediately
         * spinning it back up for a 1 KB write. Nothing happens here unless a
         * setting actually changed and the user has since gone quiet. */
        settings_commit(0);
        /* The On-The-Go list rides the same idle pass and the same gate: a
         * change made with the drive parked waits for the platters to turn
         * for some other reason, or for one of the forced commits above. */
        otg_commit(CFG_COMMIT_IDLE);
        /* The event log's idle flush, same placement for the same reason:
         * a full block lands while the platters still turn. Never wakes a
         * parked drive; at most one block per pass. */
        evlog_commit(CFG_COMMIT_IDLE);

        /* The idle spin-down, the lock park, the refused-STANDBY retry and
         * the browsing hold handed to the player: kernel/drivepower.h. */
        uint32_t now_d = mmio_read32(USEC_TIMER_ADDR);
        drivepower_in_t dp_in;
        dp_in.now_us      = now_d;
        dp_in.idle_us     = idle;
        dp_in.locked      = g_locked;
        dp_in.playing     = player_playing();
        dp_in.parked      = ata_is_parked();
        dp_in.player_busy = player_disk_busy();
        drivepower_out_t dp = drivepower_idle_feed(&g_drivepower, &dp_in);
        if (dp.standby) {
            if (dp.flush_first) {
                /* The idle path waits out a 3 s debounce and never wakes a
                 * parked drive, so a setting changed just before the lock
                 * would sit pending until the next spin-up. Write it while
                 * the platters still turn. */
                settings_commit(CFG_COMMIT_SOFT);
                otg_commit(CFG_COMMIT_SOFT);
            }
            drivepower_standby_result(&g_drivepower, ata_standby(), now_d);
        }
        player_hold_drive(dp.hold);
        disk_park_track(player_playing());

        /*
         * SELECT press-length arbitration on a LIST ROW: the tap acts on
         * release, the hold at SEL_HOLD_US adds to (or removes from)
         * On-The-Go. See the rowsel_* block for why the row is recorded at the
         * down-edge rather than read live.
         *
         * The machine is fed EVERY pass, pending or not, so a release is
         * always seen: stop feeding it after an action and the next press
         * would be timed from the old press's origin and fire HOLD instantly.
         * Only the ACTING is conditional.
         */
        {
            uint32_t nowr  = mmio_read32(USEC_TIMER_ADDR);
            int      rdown = !g_locked &&
                             (clickwheel_buttons() & WHEEL_BTN_SELECT) != 0;
            keyhold_action_t ract = keyhold_feed(&row_key, rdown, nowr,
                                                 SEL_HOLD_US);
            if (g_rowsel.pending &&
                ((int)scr_cur() != g_rowsel.scr || g_locked ||
                 g_list_epoch != g_rowsel.epoch ||
                 rowsel_sub() != g_rowsel.sub)) {
                /* The press no longer belongs to the row it started on: MENU
                 * popped the screen, the list was rebuilt under it, or Hold
                 * engaged mid-press (which zeroes clickwheel_buttons() and
                 * would read below as a release). Drop it. */
                rowsel_drop(&row_key);
            } else if (g_rowsel.pending) {
                if (ract == KEYHOLD_TAP) {
                    g_rowsel.pending = 0;
                    int r = row_select_tap(fs);
                    if (r & ROWSEL_DIRTY)   dirty = 1;
                    if (r & ROWSEL_PLAYING) np_first = 1;
                } else if (ract == KEYHOLD_HOLD) {
                    g_rowsel.pending = 0;
                    if (row_select_hold(fs)) dirty = 1;
                }
            }
        }

        /*
         * SELECT press-length arbitration (Now Playing). Decided here rather
         * than at the down-edge because the length is only known once the button
         * is released or the hold threshold passes. Held past SEL_HOLD_US opens
         * the queue immediately (so the gesture confirms itself under your
         * thumb); released before it toggles the scrubber.
         */
        if (g_sel_pending && (scr_cur() != SCR_NOWPLAYING || g_locked)) {
            /* The press no longer belongs to Now Playing: MENU popped it, the
             * track ended and truncated the stack, or Hold engaged mid-press.
             * Hold is the nasty one — it zeroes clickwheel_buttons(), which
             * reads below as a release and would scrub_enter() while locked;
             * MENU-then-release used to toggle the scrubber on whatever
             * screen was current, and the next Now Playing wheel turn then
             * seeked instead of changing the volume. Drop it, and the
             * scrubber with it. */
            g_sel_pending = 0;
            if (np_scrubbing()) {
                scrub_exit();
                np_last = 0xFFFFFFFFu;
            }
        }
        if (g_sel_pending) {
            uint32_t sel_held = mmio_read32(USEC_TIMER_ADDR) - g_sel_down_us;
            int      down     = (clickwheel_buttons() & WHEEL_BTN_SELECT) != 0;
            if (down && sel_held >= SEL_HOLD_US) {
                g_sel_pending = 0;
                scrub_exit();
                g_queue_sel   = player_queue_current();
                g_queue_accum = 0;
                scr_push(SCR_QUEUE);
                dirty = 1;
            } else if (!down) {
                g_sel_pending = 0;
                if (np_scrubbing()) scrub_exit();
                else                scrub_enter();
                /* Nothing above the transport band knows about the scrubber
                 * (nowplaying_render only consults it there), so a forced
                 * transport repaint is the whole change — no `dirty`. */
                np_last = 0xFFFFFFFFu;
            }
        }

        /*
         * Deferred seek. The target is committed once the wheel has been quiet
         * for SCRUB_COMMIT_US, so dragging across a track costs ONE seek rather
         * than one per detent — a seek stops the DAC, re-primes the ring, and on
         * a backward jump rewinds the file. Scrub mode then lapses on its own,
         * so the wheel cannot be left silently controlling position.
         */
        if (np_scrubbing()) {
            uint32_t quiet = mmio_read32(USEC_TIMER_ADDR) - g_scrub_last_us;
            if (g_scrub_dirty && quiet >= SCRUB_COMMIT_US) {
                g_scrub_dirty = 0;
                if (player_seek_to(g_scrub_target_s) != 0) {
                    g_scrub_target_s = player_elapsed_s();   /* refused: snap back */
                }
                np_last = 0xFFFFFFFFu;          /* transport band only, as above */
            } else if (!g_scrub_dirty && quiet >= SCRUB_EXIT_US) {
                scrub_exit();
                np_last = 0xFFFFFFFFu;
            }
            if (!player_active()) {
                scrub_exit();                    /* track ended under the scrubber */
            }
        }

        /* Panel wake: the instant we leave the fully-off state, bring the panel
         * back BEFORE any other present can happen this pass (the Hold-banner
         * flash or the render block below), in the only order that is safe and
         * the same one suspend_to_ram / enter_standby use:
         *
         *   lcd_wake()            panel-enable bits back, absorb armed;
         *   paint_current_screen  the real screen, rendered while still dark;
         *   lcd_present_fb        a FULL frame — its LCD_UPDATE carries the
         *                         BCM's ~500 ms panel init and the call does
         *                         not return until that has retired;
         *   backlight_set         only now, so the init is never seen (light
         *                         over it is the white flash, 02-lcd.md).
         *
         * FULL on purpose: every present while slept was refused, so the BCM's
         * framebuffer holds the last frame from BEFORE the sleep, and a partial
         * (two list rows, the transport band) on top of that would leave stale
         * pixels everywhere else. One central block covers every wake path, so
         * no input site lights the LED itself when panel_slept is set.
         *
         * dirty stays set: the render block below repaints once more through
         * the ordinary path, which is what re-registers the marquee, the
         * partial-paint caches (g_lp, np_last) and the toast/A-Z edges. That
         * second frame is invisible (same pixels) and costs one present; it is
         * exactly what suspend's return does too. */
        if (panel_slept && bl_state != BL_OFF) {
            uint32_t refused = lcd_presents_refused() - panel_refused_at_sleep;
            lcd_wake();
            panel_slept = 0;
            paint_current_screen();
            lcd_present_fb(console_framebuffer());
            backlight_set(g_settings.backlight_bright);
            dirty = 1;                    /* never resume onto a stale panel */
            if (refused) {
                /* Something presented into the slept panel. Not a fault (the
                 * frame above made up for it), but on the idle path nothing
                 * should — say so, so the device tells us who. */
                uart_puts("core: panel wake: ");
                uart_dec((int)refused);
                uart_puts(" present(s) refused while slept\n");
            }
        }

        /* A two-press confirm that lapsed unpressed has to un-say itself: the
         * row's label reverts to "Clear Playlist", so the screen it is on has
         * to repaint. ui_window_up DISARMS an expired window, so polling it
         * here is the edge — without this the row kept saying "Select again"
         * until something else happened to redraw it. */
        if (g_otg_confirm.armed && !otg_confirm_up(&g_otg_confirm)) dirty = 1;
        if (g_pl_confirm.armed  && !otg_confirm_up(&g_pl_confirm))  dirty = 1;

        /* The Hold banner holds the top chrome for ~1s on a Hold edge (and on
         * a button press while locked). Paint the context + banner once, hold
         * it, then repaint underneath when it fades. Suppresses the normal
         * render while up — which is why the input block above disarms the
         * window on the first event after an UNLOCK: the banner would otherwise
         * eat the render for the rest of its second while happily applying
         * every scroll and press behind it. The paint is always full; the PRESENT is the band
         * alone unless something else is pending — a Hold edge marks the strip
         * dirty for the padlock, and a wake from dark has a stale panel — in
         * which case the whole frame goes, as it always did. */
        uint32_t now_us = mmio_read32(USEC_TIMER_ADDR);
        /* Two banners can want the band at once (a Hold edge during an
         * On-The-Go confirmation). The lock one wins — it reports a state
         * change the user must see — and the other expires underneath;
         * nothing is applied unseen either way, because the input drain
         * disarms the On-The-Go banner on the first press. Both windows are
         * polled exactly once: ui_window_up DISARMS an expired one.
         *
         * A dark modal hosts neither: a window armed on the screen under it
         * (a Hold flip, then the cable, inside one second) is dropped here
         * rather than painted as a linen strip over the field — and so
         * cannot be replayed on the screen the modal pops back to. */
        if (!banner_hosted()) {
            g_lock_flash.armed = 0;
            g_otg_flash.armed  = 0;
        }
        int lock_up = ui_window_up(&g_lock_flash, LOCK_FLASH_US, now_us);
        int otg_up  = ui_window_up(&g_otg_flash,  OTG_FLASH_US,  now_us);
        if (lock_up || otg_up) {
            if (!banner_up && bl_state != BL_OFF) {
                paint_current_screen();
                if (lock_up) lock_banner_render(g_locked);
                else         otg_banner_render();
                if (dirty) {
                    lcd_present_fb(console_framebuffer());
                    dirty = 0;
                } else {
                    lcd_present_rect(console_framebuffer(), 0, 0, LCD_WIDTH,
                                     top_banner_h());
                }
            }
            banner_up = 1;
            /* The banner skips the render below — and with it the loop's two
             * halts, so it halts here itself. Idle (which includes paused: no
             * DMA to pace) parks the core for a tick, as the main 10 ms halt
             * does. PLAYING takes the same 200 us halt as the bottom of the
             * loop, under the same "the pump did nothing" gate: without it a
             * Hold flip mid-track free-spun the core at 80 MHz for the whole
             * second the banner is up, re-polling the wheel through masked IRQs
             * — the exact spin the bottom halt exists to prevent. */
            if (!player_playing()) {
                cpu_wait_ms(10);
            } else if (pump_us < 200u) {
                cpu_wait_us(200);
            }
            continue;                     /* skip the normal render this pass      */
        }
        if (banner_up) {              /* banner just faded: repaint underneath */
            banner_up = 0;
            dirty = 1;
        }

        /* A-Z locator: while the wheel is being spun fast, show the selected
         * row's initial so you can aim at a letter instead of reading rows that
         * are flying past. Its appearance/disappearance is itself a repaint. */
        char az_letter = wheel_accelerating() ? list_sel_initial() : 0;
        /* Suppressed with the backlight: a toast nobody can see is only a
         * repaint. It reappears on the next sample if the cell is still low. */
        int  toast     = (bl_state != BL_OFF) &&
                         battwarn_toast_up(mmio_read32(USEC_TIMER_ADDR));

        /* Render the current screen (skipped entirely when the screen is off —
         * saves the IRQ-masked present while music plays dark). Now Playing
         * repaints ~1/s for the clock; menus/browser only on change. */
        if (bl_state == BL_OFF) {
            /* nothing to draw */
        } else if (scr_cur() == SCR_NOWPLAYING) {
            uint32_t nowv    = mmio_read32(USEC_TIMER_ADDR);
            uint32_t elapsed = player_elapsed_s();
            int vol_active = ui_window_up(&g_vol_show, VOL_SHOW_US, nowv);
            /* The volume plate needs a push when it appears, when the wheel
             * moves it while it is up, and when it fades (to put back what it
             * covered). None of those is a full-frame event. */
            int vol_edge   = (vol_active != np_vol_prev) ||
                             (vol_active && np_vol_dirty);

            /* A FULL repaint is needed only on a real change (dirty — which the
             * loop's track-change edge sets for us) or a toast edge. Everything
             * else is a partial: the clock tick redraws ONLY the transport strip
             * (elapsed/remaining/progress) and presents only its band, and a
             * volume edge presents only the plate rect, instead of re-rendering
             * the whole screen — art, metadata and two anti-aliased bars — and
             * pushing all 38400 words for a 3200-word plate. */
            int want_full = dirty || toast != toast_prev;
            if (want_full || vol_edge) {
                if (np_first || !player_active() ||
                    (uint32_t)(nowv - last_present) >= present_gap_us()) {
                    g_mq.active = 0;
                    console_damage_reset();
                    nowplaying_render(player_track_name(), elapsed,
                                      player_total_s(), player_buf_pct());
                    if (toast) {
                        screen_battery_toast_render();
                        battwarn_toast_shown(nowv);   /* its 4 s start here */
                        g_mq.active = 0;      /* see the list branch below */
                    }
                    toast_prev = toast;
                    if (!want_full) {
                        /* Volume edge only. The screen was re-rendered in full
                         * (the plate's anti-aliased corners blend into whatever
                         * is under them, so a fresh background is the only way
                         * to draw them — or to erase them — without drift), but
                         * everything outside the plate came out as the pixels
                         * the panel already shows. Push just the plate rect;
                         * console.c's damage rect is a union, so replace it.
                         * np_last is left alone: if the second rolled over in
                         * this pass, the next one presents the transport band. */
                        console_damage_reset();
                        console_damage_add(VOL_PLATE_X, VOL_PLATE_Y,
                                           VOL_PLATE_W, VOL_PLATE_H);
                    }
                    ui_present_damage();
                    np_first = 0;
                    if (want_full) {
                        np_last = elapsed;
                        dirty   = 0;
                    }
                    np_vol_prev  = vol_active;
                    np_vol_dirty = 0;
                    last_present = nowv;
                    player_note_presented();
                    g_lp.valid = 0;             /* not a list screen */
                }
                /* else: throttled — keep dirty / the volume edge pending, present
                 * in the next window (np_vol_prev is what makes a fade retry). */
            } else if (elapsed != np_last) {
                /* Transport only: leave the marquee registered (its own tick
                 * keeps the title scrolling) and don't touch the art/metadata.
                 * The clock lands here once a second; the scrubber lands here
                 * on every detent (it forces np_last), so THAT case is paced by
                 * the last present's cost like every other wheel-driven paint —
                 * np_last stays forced until the band actually goes out. */
                /* Paced against the last present of ANY kind, scrubbing or
                 * not: the clock's band a few ms behind a full frame found the
                 * BCM still retiring it (see ui_present_damage). np_last stays
                 * unchanged when throttled, so the band goes out next pass. */
                if ((uint32_t)(nowv - last_present) >= present_gap_us()) {
                    console_damage_reset();
                    nowplaying_transport_render(elapsed, player_total_s());
                    ui_present_damage();
                    np_last      = elapsed;
                    last_present = nowv;
                    player_note_presented();
                }
            }
        } else if (dirty || az_letter != az_prev || toast != toast_prev) {
            /* Repaints are paced by what the LAST present actually cost (see
             * present_gap_us), not by a fixed 150 ms: a selection move now
             * repaints two rows and pushes only those, so it can run several
             * times faster without keeping IRQs masked long enough to starve the
             * audio DMA ISR. Idle → present immediately. `dirty` stays set until
             * we present, so the latest scroll position is what lands. */
            uint32_t now = mmio_read32(USEC_TIMER_ADDR);
            if (!player_active() ||
                (uint32_t)(now - last_present) >= present_gap_us()) {
                g_mq.active = 0;
                console_damage_reset();
                /* Selection moved and nothing else did? Repaint the two rows
                 * (plus the header count + scrollbar) instead of clearing and
                 * redrawing the panel. Anything else falls through to the full
                 * render, which console_clear marks as whole-screen damage. The
                 * A-Z plate covers arbitrary rows, so while it is up (or in the
                 * frame that clears it) the full render is the honest option. */
                int partial = !az_letter && !az_prev && !toast && !toast_prev &&
                              list_repaint_partial();
                if (!partial) {
                    switch (scr_cur()) {
                    case SCR_MENU:    main_menu_render();            break;
                    case SCR_MUSIC:   music_menu_render();           break;
                    case SCR_ARTISTS: artists_render(g_artist_sel);  break;
                    case SCR_SONGS:   songs_render(g_song_sel);      break;
                    case SCR_GENRES:  genres_render(g_genre_sel);    break;
                    case SCR_PLAYLISTS: playlists_render(g_pl_sel);  break;
                    case SCR_PLAYLIST:  playlist_render(g_plt_sel);  break;
                    case SCR_OTG:       otg_render(g_otg_sel);       break;
                    case SCR_SEARCH:    search_render_cur();         break;
                    case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel);   break;
                    case SCR_QUEUE:   queue_render(g_queue_sel);     break;
                    case SCR_SETTINGS: settings_render_cur();        break;
                    case SCR_BATTERY:
                        battery_modal_paint();
                        break;
                    case SCR_CHARGING:
                        charging_screen_paint();
                        break;
                    default: break;                 /* NOWPLAYING handled above */
                    }
                }
                if (az_letter) az_overlay_render(az_letter);
                if (toast) {
                    screen_battery_toast_render();
                    battwarn_toast_shown(now);        /* its 4 s start here */
                }
                /* The plate owns the screen. The repaint above re-registered
                 * the selected row with the marquee, whose tick would then
                 * punch a hole straight through whatever is covering it. */
                if (az_letter || toast) g_mq.active = 0;
                /* Render and present timed apart: `now` was read before the
                 * paint, so the delta here is the CPU the renderer took. */
                g_ui_render_us = mmio_read32(USEC_TIMER_ADDR) - now;
                if (g_ui_render_us > g_ui_render_max_us) g_ui_render_max_us = g_ui_render_us;
                ui_present_damage();
                if (g_present_cost_us > g_ui_present_max_us) g_ui_present_max_us = g_present_cost_us;
                if (partial) g_ui_partial++; else g_ui_full++;
                list_paint_note();
                az_prev = az_letter;
                toast_prev = toast;
                dirty = 0;
                last_present = now;
            }
        }

        /* Animate the now-playing 3-bar indicator in the album detail: redraw
         * only its ~10px box and partial-present just that, ~9fps. A tiny pixel
         * push (unlike a full repaint) so it can't starve the audio DMA. Not
         * while paused: the bars stand still (bars_clock) and nothing need be
         * pushed. */
        if (bl_state != BL_OFF && scr_cur() == SCR_BROWSER && g_dir_depth > 0
            && player_active() && !player_paused()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - last_bars) >= 110000u) {
                int sv = 0;
                for (int i = 0; i < g_det_view_n; i++)
                    if (g_det_view[i] == (int16_t)g_det_sel) { sv = i; break; }
                int top = ui_scroll_window(sv, g_det_view_n, DET_ROWS);
                const char *pn = player_track_name();
                for (int r = 0; r < DET_ROWS; r++) {
                    int vi = top + r;
                    if (vi >= g_det_view_n) break;
                    int16_t v = g_det_view[vi];
                    if (v < 0) continue;
                    const browse_entry_t *e = &g_browse[v];
                    if (e->is_dir || !name_eq_ci(e->name, pn)) continue;
                    int ry = DET_LIST_Y0 + r * ROW_H;
                    int is_sel = (v == g_det_sel);
                    console_fill_rect(15, ry + 6, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK,
                                    bars_clock());
                    lcd_present_rect(console_framebuffer(),
                                     15, ry + 6, NP_BARS_W + 1, NP_BARS_H);
                    break;
                }
                last_bars = nowb;
            }
        }

        /* Same animated bars on the queue view (the currently-playing row). */
        if (bl_state != BL_OFF && scr_cur() == SCR_QUEUE && player_active()
            && !player_paused()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - last_bars) >= 110000u) {
                int n = player_queue_len();
                int cur = player_queue_current();
                int top = ui_scroll_window(g_queue_sel, n, LIST_ROWS);
                int r = cur - top;
                if (cur < n && r >= 0 && r < LIST_ROWS) {
                    int ry = LIST_Y0 + r * ROW_H;
                    int bx = LCD_WIDTH - 22, by = ry + 7;
                    int is_sel = (cur == g_queue_sel);
                    console_fill_rect(bx, by, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(bx, by, is_sel ? LINEN_SEL_FG : LINEN_INK,
                                    bars_clock());
                    lcd_present_rect(console_framebuffer(),
                                     bx, by, NP_BARS_W + 1, NP_BARS_H);
                }
                last_bars = nowb;
            }
        }

        /* Scroll the marquee target (a selected long row, or the now-playing
         * title) in place — redraw just its band + partial-present it, ~16fps. */
        if (bl_state != BL_OFF && g_mq.active) {
            uint32_t nowm = mmio_read32(USEC_TIMER_ADDR);
            /* Phase clock (g_mq.t0) + target-change detection live in mq_set, so
             * this just paces the redraw at the shared live offset. */
            if ((uint32_t)(nowm - last_mq) >= 33000u) {   /* ~30fps               */
                draw_marquee(g_mq.x, g_mq.y, g_mq.w, g_mq.text, g_mq.tw, g_mq.font,
                             g_mq.ink, g_mq.bg, nowm - g_mq.t0, g_mq.cy0, g_mq.cy1);
                int ptop = g_mq.y - text_ascent(g_mq.font);
                int pbot = g_mq.y + text_descent(g_mq.font);
                if (ptop < 0) ptop = 0;
                lcd_present_rect(console_framebuffer(),
                                 g_mq.x, ptop, g_mq.w, pbot - ptop);
                last_mq = nowm;
            }
        }

        /* Load album covers while on the album list. Idle → blast several per
         * pass (covers fill near-instantly). While a song plays we still spread
         * the I/O so it can't starve decode, but a folder.thm is only ~1KB and
         * the anti-skip diskbuf is 8MB (~73s), so a small BATCH every ~60ms
         * (~50 covers/s) is safe and stops the old one-line-at-a-time trickle.
         *
         * Every load is a SYNCHRONOUS disk read on this loop, so two things are
         * held back while the wheel is actually turning:
         *
         *   - A PARKED platter is not woken for a cover. player.c parks the
         *     drive between refill bursts (most of a track) and the idle timer
         *     parks it after 20 s, so the first cover read after either is a
         *     spin-up: the loop is frozen for the 1-3 s it takes (ATA_SPINUP_US
         *     allows 4), the wheel's motion piles into the latch, and the list
         *     jumps when it comes back. Covers wait for CHIP_SPINUP_QUIET_US of
         *     wheel silence, so that stall lands on a still screen — a wait, not
         *     a jerk — and a slow deliberate scroll never triggers it at all.
         *   - With the platter up, a scroll in progress gets ONE read per pass.
         *     Six reads is ~60 ms of seek+PIO stacked onto the pass that also
         *     has to paint the new window; one bounds the hitch and the rest of
         *     the window fills the moment the wheel settles.
         *
         * The pump is told the window (artcache_pump_for), so whichever reads
         * do happen go to the rows on screen, top first — never to rows that
         * already scrolled past. */
        if (scr_cur() == SCR_BROWSER && g_dir_depth == 0) {
            uint32_t nowc = mmio_read32(USEC_TIMER_ADDR);
            int idle_now  = !player_active();
            uint32_t since_wheel = nowc - wheel_last_us();
            int moving = since_wheel < CHIP_WHEEL_SETTLE_US;
            int budget;
            if (ata_is_parked() && g_locked) {
                /* Locked: the user has said they are done with the menus,
                 * and the lock parks the drive. Waking it for a cover here
                 * was a spin-up and a park per batch. */
                budget = 0;
            } else if (ata_is_parked() && since_wheel < CHIP_SPINUP_QUIET_US) {
                budget = 0;
                g_ui_art_held++;
            } else if (moving) {
                budget = 1;
            } else {
                budget = idle_now ? 6 : 3;
            }
            if (budget > 0 && (idle_now || (uint32_t)(nowc - last_chip) >= 60000u)) {
                /* A pumped slot is only worth a repaint if it is actually ON
                 * SCREEN: pumping a whole library's covers used to set dirty for
                 * every one of them, forcing ~21 consecutive full-frame repaints
                 * for rows nobody was looking at. Snapshot the visible rows'
                 * chips, pump, and repaint only the rows whose chip appeared —
                 * through g_lp.rows, so a landed cover costs one row band, not a
                 * cleared panel and a full-frame push (see list_repaint_partial). */
                const uint16_t *before[LIST_ROWS2];
                int want[LIST_ROWS2 + 2 * CHIP_AHEAD_ROWS];
                int nlist = albumlist_count();
                int vtop = ui_scroll_window(g_br_sel, nlist, LIST_ROWS2);
                /* PEEK, not get: this loop is only observing. artcache_get
                 * claims a way and re-stamps the LRU, so using it here (12
                 * times a pass, always in row order) is what pinned the top
                 * row to the lowest stamp and starved it. The render's own
                 * artcache_get calls are what should drive residency. */
                /* Map row -> global album index: the cache is keyed by album,
                 * and a row is not an album when an artist filter puts the
                 * synthetic "All Songs" row at 0. */
                for (int r = 0; r < LIST_ROWS2; r++) {
                    want[r]   = albumlist_album_at(vtop + r);
                    before[r] = (want[r] >= 0) ? artcache_peek(want[r]) : 0;
                }
                /* Then the look-ahead, after the window so it never delays a
                 * visible chip: a screen in the direction the selection last
                 * moved, then a screen the other way. Only from a drive that
                 * is already turning: rows nobody is looking at are not worth
                 * a spin-up (or, idle, the battery a spin-up costs). */
                int nwant = LIST_ROWS2;
                if (g_br_sel != g_chip_last_sel) {
                    g_chip_dir = (g_br_sel > g_chip_last_sel) ? 1 : -1;
                    g_chip_last_sel = g_br_sel;
                }
                if (!ata_is_parked()) {
                    for (int pass = 0; pass < 2; pass++) {
                        int dir = pass == 0 ? g_chip_dir : -g_chip_dir;
                        for (int k = 0; k < CHIP_AHEAD_ROWS; k++) {
                            int row = dir > 0 ? vtop + LIST_ROWS2 + k
                                              : vtop - 1 - k;
                            if (row < 0 || row >= nlist) break;
                            want[nwant++] = albumlist_album_at(row);
                        }
                    }
                }
                uint32_t t_art = mmio_read32(USEC_TIMER_ADDR);
                int loaded = 0;
                while (loaded < budget && artcache_pump_for(fs, want, nwant)) {
                    loaded++;
                }
                t_art = mmio_read32(USEC_TIMER_ADDR) - t_art;
                g_ui_art_reads += (uint32_t)loaded;
                if (loaded) drivepower_note_ui_read(&g_drivepower, mmio_read32(USEC_TIMER_ADDR));
                if (loaded && t_art > g_ui_art_max_us) g_ui_art_max_us = t_art;
                for (int r = 0; r < LIST_ROWS2; r++) {
                    const uint16_t *now_px = (want[r] >= 0) ? artcache_peek(want[r]) : 0;
                    if (now_px != before[r]) {
                        g_lp.rows |= 1u << r;
                        dirty = 1;
                    }
                }
                last_chip = nowc;
            }
        }

        /* The scroll diagnostics line, on the same 5 s cadence as the battery
         * and audio lines but only when something was painted. */
        {
            uint32_t nows = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nows - last_uistat) >= 5000000u) {
                ui_stats_emit();
                last_uistat = nows;
            }
        }

        /* While PLAYING, a pass that did nothing — no decode work, no repaint —
         * used to free-spin the core at 80 MHz, re-polling the wheel (which masks
         * and unmasks IRQs) thousands of times a second. Halt instead. The halt
         * is 200 us, NOT milliseconds: the countdown is not documented to wake on
         * an interrupt, and the audio DMA ISR has only the ~363 us I2S FIFO of
         * slack — 200 us stays inside that budget while still parking the core
         * for the overwhelming majority of an idle pass. Input is latched by the
         * tick ISR and every animation here ticks at >= 33 ms, so nothing is lost.
         *
         * Gated on player_playing(), not player_active(). The 200 us figure is a
         * DMA-feeding budget, and a PAUSED player has no DMA to feed — but
         * active() stays 1 across a pause, so a paused device used to take this
         * branch and spin the loop ~5,000 times a second (each pass a
         * player_pump call, several USEC_TIMER reads, GPIO reads and an event
         * drain) for as long as it sat paused. Paused is idle: it belongs in
         * the 10 ms halt below, fifty times fewer wakeups. */
        if (player_playing() && !dirty && pump_us < 200u) {
            cpu_wait_us(200);
        }

        /* Only throttle when idle; while playing, player_pump's decode_step
         * paces the loop and the wheel stays responsive. Halt the core until the
         * next tick (self-waking ~10 ms) instead of a busy-spin: input is latched
         * by the 100 Hz timer ISR, so this costs no responsiveness. "Idle"
         * includes paused (see above); the paused pump's only job — the codec
         * power-down timeout — is seconds long, so 100 Hz is ample for it. */
        if (!player_playing()) {
            cpu_wait_ms(10);
        }

        /*
         * Supervisor stack guard. crt0 paints the region below _stack_limit, so
         * this is an O(1) look at the low words — cheap enough to run every idle
         * pass. A breach means a deep UI call chain has already written past the
         * bottom of the stack, which is silent memory corruption; panic() at
         * least turns it into a readable screen with the faulting PC instead of
         * a mystery freeze or garbled state hours later.
         */
        if (stack_guard_breached()) {
            panic(PANIC_STACK, 0);
        }
    }
}

/* Per-task stack for the idle task (carved from .bss). Only reached if the LCD
 * is unpowered or the disk won't mount — the browser above never returns. */
#define TASK_STACK_SIZE 1024
static uint8_t idle_stack[TASK_STACK_SIZE];

/* Disk scratch (uint16_t for the 2-byte alignment ata_read_sectors needs). */
static uint16_t mbr_sector[1024];

/*
 * Idle task: never exits. Sleep the CPU for a short countdown, then yield.
 * The scheduler is only started on the no-LCD / mount-failure fallback path.
 */
_Noreturn static void idle_task(void) {
    uart_puts("core: idle task entered\n");
    for (;;) {
        cpu_wait_ms(10);
        sched_yield();
    }
}

_Noreturn void kernel_main(void) {
    /* Free-running 1 MHz microsecond counter (USEC_TIMER, 01-soc-pp5022.md
     * "Timers"), sampled at the very top so later milestones can report
     * elapsed-since-boot in real time, INDEPENDENT of the 100 Hz tick. */
    uint32_t boot_us0 = mmio_read32(USEC_TIMER_ADDR);

    uart_init();

    uart_puts("core: kernel alive (iPod 5G/5.5G, PP5022)\n");
    /* The build, said by the build itself. Every byte the UART sends is also
     * captured into the event log (evlog_capture), so once this reaches the
     * disk the log can name which firmware a boot ran — the host otherwise
     * only knows what it WROTE (COREFLSH.TXT) and infers the rest. A plain
     * string, deliberately without the CORE-FW-VERSION: prefix: the image
     * must carry exactly one marker for check_version_marker.sh to find. */
    uart_puts("core: version " CORE_VERSION " build " CORE_BUILD_ID "\n");

    /*
     * Hand the shared list chrome its scrolling-title implementation.
     *
     * ui_list_row() draws an overflowing title through this hook. The marquee
     * needs USEC_TIMER, and a time source is exactly what ui/chrome.c must not
     * reach for if it is to build on the host — so the dependency is injected
     * here instead. Registered once, before anything can paint; leave it unset
     * (as the host tests do) and titles are plainly clipped.
     */
    ui_set_scroll_text(mq_text);

    /*
     * The wheel's acceleration state machine (ui/wheel.c) is portable for the
     * same reason and by the same means: its clock, its "what letter is row N
     * on this screen" source and its navigation click are injected here rather
     * than reached for. Unset, none of them can misbehave — no clock reads 0,
     * no letter source means nothing letter-steps, no click is silent.
     */
    wheel_set_clock(wheel_clock);
    wheel_set_initial_at(list_initial_at);
    wheel_set_letter_step(list_letter_step_idx);
    /* .bss zeros happen to be Search's start state (PICK, the cursor on 'A',
     * an empty query); saying so means a field added to search_t later cannot
     * quietly start as something else. */
    search_reset(&g_search);
    wheel_set_click(ui_click);

    /* Hex-path self-test: if this doesn't read 1234ABCD on the terminal,
     * distrust every register dump that follows. */
    uart_puts("core: uart self-test ");
    uart_put_hex32(0x1234ABCD);
    uart_putc('\n');

    uart_puts("core: PROCESSOR_ID ");
    uart_put_hex32(mmio_read32(PROCESSOR_ID_ADDR));   /* through the seam: same load on hw */
    uart_putc('\n');

    /* Come off the 24 MHz boot clock up to CPUFREQ_NORMAL (30 MHz) before the
     * rest of bring-up so it runs at a sane speed. */
    uart_puts("core: clock init -> 30 MHz\n");
    clock_init();
    uart_puts("core: cpu freq ");
    uart_put_hex32(cpu_frequency());
    uart_putc('\n');

    /* Turn on the unified cache now — decode is far too slow with it off.
     * Write-back, so the audio DMA path flushes (cache_commit) before the DMA
     * reads a freshly-filled buffer. */
    uart_puts("core: cache init\n");
    cache_init();

    /* 100 Hz system tick + core IRQ unmask BEFORE audio: continuous playback is
     * DMA-driven and needs its completion interrupt delivered, and sleep_ms
     * spins on the IRQ-fed tick. */
    uart_puts("core: timer init @ 100 Hz, enabling IRQs\n");
    timer_init();
    arch_irq_enable();

    /* Boot clock starts here — the earliest point with a running timer. */
    g_boot_t0_us = mmio_read32(USEC_TIMER_ADDR);

    /* (A tick self-test used to sleep 100 ms here to prove the IRQ-fed tick
     * advanced. It did its job during bring-up; on a cold boot it is 100 ms of
     * a 10 s startup spent proving something the scheduler exercises anyway.) */

    /*
     * Backlight FIRST — before the LCD probe, deliberately.
     *
     * It used to live inside the lcd_init() block, which made it invisible
     * whether a dark panel meant "our code never ran" or "our code ran and
     * lcd_init() failed". Chainloaded from ipodloader2 that never mattered:
     * the loader draws its own menu, so it has already lit the backlight and
     * brought the BCM up before handing off. Booting DIRECTLY from the
     * firmware partition, nobody has — and a cold-BCM lcd_init() failure
     * skipped the backlight, the disk and the UI in one go, which is
     * indistinguishable from a crash in crt0 (2026-07-26: black screen, cause
     * undiagnosable for exactly this reason).
     *
     * Nothing here depends on the LCD: backlight_init() is GPIO B/D/L only
     * (charge pump, step line, LED enable) and its delay loops self-scale to
     * the live core clock via cpu_frequency(), whose 30 MHz calibration point
     * is precisely where we are now — before cpu_boost(), not after.
     *
     * Cost: the panel is lit for the duration of lcd_init() (up to ~500 ms of
     * BCM bring-up) before the boot screen paints, so a cold boot can show a
     * brief bright field. That is the price of being able to see a boot at
     * all, and it is one line to put back.
     */
    backlight_init();

    /* LCD probe gates the whole disk/audio/UI stack: nonzero BCM power => real
     * hardware. The clicky emulator has no BCM (lcd_init() false there), so
     * this block is skipped, keeping the emulator smoke green; the register
     * grammar is proven host-side by the mock-bus trace tests. */
    uint32_t lcd_t0 = boot_ms_now();
    int      lcd_ok = lcd_init();
    g_boot_lcd_ms   = boot_ms_now() - lcd_t0;
    if (lcd_ok) {
        /* Boost to 80 MHz for the whole disk/decode path. The UI runs here too;
         * we never drop back because the browser never returns (fine for
         * bring-up — the device is on a cable during testing). */
        cpu_boost();

        /* Bring up the I2C control bus now (not just at first-song hal_audio_init)
         * so the status strip can read the PCF50605 battery gauge from the menu
         * — and so run_ui's boot block can read the same chip's clock, which it
         * does before anything paints a time. Bounded/idempotent —
         * hal_audio_init re-inits it harmlessly per track. */
        i2c_init();
        battery_init();
        hal_volume_init();               /* codec output gain -> safe default      */
        piezo_init();                    /* PWM click for menu navigation          */
        hal_audio_boot_quiet();          /* codec cold + muted until the first     *
                                          * track: a Menu+Select reset keeps its   *
                                          * rails up, and an inherited live codec  *
                                          * hisses on the jack with nothing playing */

        /* Paint the boot screen immediately, so the panel shows CORE branding
         * instead of whatever the panel held while the disk spins up and the
         * volume mounts. No bar yet — nothing measurable has started. This one
         * paint is necessarily the default palette; run_ui repaints it in the
         * user's theme as soon as the settings are read. */
        boot_screen_render("LOADING", -1);

        /* Read the MBR, find the FAT32 data partition (type 0B/0C), mount it. */
        uint8_t *mbr = (uint8_t *)mbr_sector;
        uint32_t sig = 0, fat_lba = 0;
        int      mnt = -1;
        fat32_t  fs;
        /* Everything from here to the end of fat32_mount is "the disk": the
         * IDE reset, the first read (which is what actually spins the platters
         * up, and is allowed 4 s for it — see ATA_SPINUP_US), and the mount's
         * own reads. Timed as one phase because that is the unit we could move
         * or overlap; splitting it finer would not change what we can do. */
        uint32_t disk_t0 = boot_ms_now();
        if (ata_init() == 0 && ata_read_sectors(0, 1, mbr) == 0) {
            sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
            for (int p = 0; p < 4; p++) {
                const uint8_t *e = &mbr[0x1BE + 16 * p];
                if (e[4] == 0x0B || e[4] == 0x0C) {
                    fat_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                              ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                    break;
                }
            }
        }
        if (sig == 0xAA55u && fat_lba != 0) {
            /* The MBR start-LBA may be in native 512-byte units OR (stock 80 GB,
             * 2048-byte FAT sectors) in 2048-byte units. Try as-is, then x4;
             * fat32_mount validates the boot signature + BPB. */
            mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba);
            if (mnt != 0) {
                mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba * 4u);
            }
        }

        g_boot_disk_ms = boot_ms_now() - disk_t0;

        uint32_t btus = mmio_read32(USEC_TIMER_ADDR) - boot_us0;
        uart_puts("core: mount rc ");
        uart_put_hex32((uint32_t)mnt);
        uart_puts(" BTUS ");
        uart_put_hex32(btus);
        uart_putc('\n');

        if (mnt == 0) {
            /* Capacity / free (MB) for the About screen (64-bit to avoid the
             * clusters*bytes overflow on a big volume). */
            g_total_mb = (uint32_t)(((uint64_t)fs.total_clus * fs.clus_bytes) >> 20);
            g_free_mb  = (fs.free_clus == 0xFFFFFFFFu) ? 0xFFFFFFFFu
                       : (uint32_t)(((uint64_t)fs.free_clus * fs.clus_bytes) >> 20);
            uart_puts("core: entering browser\n");
            run_ui(&fs);                       /* never returns */
        }

        /* Mount failed: show a diagnostic error screen and fall through to the
         * idle scheduler so the device isn't a black brick. */
        console_clear(CON_RED);
        console_str  (2, 3, "MOUNT FAILED", CON_WHITE, CON_RED);
        console_str  (2, 6, "SIG",  CON_WHITE, CON_RED);
        console_hex32(8, 6, sig,               CON_WHITE, CON_RED);
        console_str  (2, 8, "MNT",  CON_WHITE, CON_RED);
        console_hex32(8, 8, (uint32_t)mnt,     CON_WHITE, CON_RED);
        console_str  (2, 10, "PART", CON_WHITE, CON_RED);
        console_hex32(8, 10, fat_lba,          CON_WHITE, CON_RED);
        lcd_present_fb(console_framebuffer());
        cpu_unboost();
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping disk + UI\n");
    }

    /* Fallback only (no LCD or mount failure): idle the core under the
     * cooperative scheduler. sched_start never returns. */
    uart_puts("core: sched init (idle fallback)\n");
    sched_init();
    if (sched_add_task(idle_task, idle_stack, sizeof idle_stack, "idle") < 0) {
        uart_puts("core: FATAL sched_add_task failed\n");
        for (;;) {
        }
    }
    sched_start();
}
