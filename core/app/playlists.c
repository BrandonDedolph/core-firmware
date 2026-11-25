/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/playlists.c — see playlists.h.
 */

#include <stdint.h>
#include "hw/uart.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../kernel/resume_ctx.h"
#include "../ui/text.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "../library/playlist.h"
#include "../library/otg.h"
#include "../library/otg_slot.h"
#include "state.h"
#include "layout.h"
#include "util.h"
#include "nav.h"
#include "library.h"
#include "playctx.h"
#include "statusbar.h"
#include "main_internal.h"
#include "playlists.h"

/* ---------------------------------------------------------------------------
 * Playlists (Music -> Playlists): the .m3u8 files under Music/Playlists,
 * listed by name, and one of them opened into a tracklist that plays as a
 * queue. library/playlist.c does the reading and resolving (host-tested);
 * this section binds each row to its library record — title, artist,
 * duration, cover — by the same test resolve_art_cb applies to a directory
 * entry, and hands the rows to the player. Caps and the empty states are
 * the library's (PLAYLIST_MAX playlists, PLAYLIST_TRACKS_MAX rows).
 * ------------------------------------------------------------------------- */
playlist_t         g_playlists[PLAYLIST_MAX];
int                g_playlists_n;
int                g_playlists_err;      /* FAT32_* when the list could
                                                 * not be read (not "none")    */
static int                g_playlists_truncated;
int                g_pl_sel, g_pl_accum; /* Playlists list             */
/* Whether the Playlists folder has been read since boot. Search wants playlist
 * NAMES without making the user visit the list first, and the disk is
 * read-only while the firmware runs, so one read a session is exact — the
 * alternative is spinning a parked drive on every Search entry. */
int                g_playlists_scanned;

playlist_track_t   g_pl_tracks[PLAYLIST_TRACKS_MAX];
int16_t            g_pl_song[PLAYLIST_TRACKS_MAX]; /* g_songs index, -1 */
int                g_pl_tracks_n;
int                g_pl_open = -1;       /* g_playlists index shown    */
static int                g_pl_err;             /* negative: file unreadable  */
static playlist_stats_t   g_pl_stats;
int                g_plt_sel, g_plt_accum; /* tracklist                */
int                g_pl_slot;    /* 1..OTG_SLOTS when the open playlist
                                         * IS a saved On-The-Go slot, else 0  */
static int                g_pl_damaged; /* ...and its save was torn           */
static playlist_scratch_t g_pl_scratch;         /* ~35 KB of .bss, one copy   */

/* Where the playlists folder is looked for, and what a relative entry in a
 * playlist is relative to: the library root (Music/ when it exists, else
 * the volume root — lib_root() found which at load). */
uint32_t playlists_root_clus(fat32_t *fs)
{
    return g_lib_root_clus ? g_lib_root_clus : fs->root_clus;
}
static const char *playlists_base_dir(void)
{
    return g_lib_root_clus ? "Music/" PLAYLIST_DIR : PLAYLIST_DIR;
}

/* (Re)read the Playlists folder into g_playlists. The list screen reads it
 * once a boot (playlists_ensure): a playlist copied over USB arrives through
 * Disk Mode, which reboots, so the folder cannot change under a running
 * firmware except by the On-The-Go save and delete below, and both re-read
 * it here. */
void playlists_load(fat32_t *fs)
{
    g_list_epoch++;
    library_ensure(fs);                   /* sets g_lib_root_clus, binds songs */
    uint32_t dir = 0;
    /* hide_empty_slots = 1: the five On-The-Go slot files always exist (the
     * host creates them — the firmware cannot), and an EMPTY one is not a
     * playlist the user made. A used, damaged or foreign one is listed. */
    int n = playlist_scan(fs, playlists_root_clus(fs), g_playlists,
                          PLAYLIST_MAX, &dir, &g_playlists_truncated, 1);
    g_playlists_err = (n < 0) ? n : 0;
    g_playlists_n   = (n < 0) ? 0 : n;
    /* Only a read that WORKED counts as the session's one read. A transient
     * disk error would otherwise leave Search with no playlist hits until the
     * next boot, while Music > Playlists — which re-reads on every entry —
     * recovered on the next look. */
    g_playlists_scanned = (n >= 0);
    if (g_pl_sel >= g_playlists_n + 1) {      /* + the pinned On-The-Go row */
        g_pl_sel = 0;                     /* the row under the cursor is gone */
    }
    g_pl_accum = 0;
}

/* Entering the Playlists screen. The folder read used to happen on every
 * entry, and with the drive parked that was a spin-up before the list
 * appeared. Only a read that worked is kept (g_playlists_scanned), so a
 * transient error is still retried on the next entry. */
void playlists_ensure(fat32_t *fs)
{
    if (!g_playlists_scanned) {
        playlists_load(fs);
        return;
    }
    g_pl_accum = 0;                       /* what the load would have reset */
}

/*
 * Which library song a playlist row IS — the same test resolve_art_cb
 * applies when it binds a record to a directory entry: the folded hash of
 * the FULL on-disk name equals the record's file_hash, within the same
 * album folder. Then, when the record has bound to a cluster, that cluster
 * must be this file's — a re-import can leave a fresh copy under an old
 * name. -1 for a file the index has no record of: it plays all the same,
 * it just shows its filename and no duration.
 */
static int playlist_bind_row(const playlist_track_t *t)
{
    uint32_t fh = t->file_hash;
    if (fh == 0) {
        return -1;                        /* lossy name: no locator */
    }
    int pick = -1;
    for (int i = g_song_hh[fh & (SONG_HASH_BUCKETS - 1)]; i; i = g_song_hn[i - 1]) {
        const lib_song_t *s = &g_songs[i - 1];
        if (s->file_hash != fh || s->dir_clus != t->dir_clus) continue;
        if (s->file_clus != 0 && s->file_clus != t->clus) continue;
        if (pick < 0) {
            pick = i - 1;
            if (s->file_clus != 0) break;     /* bound: the cluster settled it */
            continue;
        }
        /* A second unbound candidate in the same folder — two files whose
         * names fold to one hash ("It's" / "It’s"). The exact on-disk name
         * decides, as resolve_art_cb does; otherwise the first in the chain. */
        if (name_bind_exact(s->file, t->name, 0)) { pick = i - 1; break; }
        if (name_bind_exact(g_songs[pick].file, t->name, 0)) break;
    }
    return pick;
}

/* Copy the path component at *p (up to the next '/' or '\\' or the end)
 * into dst, NUL-terminated, and step *p past its separator. Returns its
 * length, or -1 when it is empty or does not fit (cap includes the NUL). */
static int pl_path_part(const char **p, char *dst, int cap)
{
    int n = 0;
    const char *s = *p;
    while (*s && *s != '/' && *s != '\\') {
        if (n + 1 >= cap) return -1;
        dst[n++] = *s++;
    }
    dst[n] = '\0';
    if (*s) s++;
    *p = s;
    return n > 0 ? n : -1;
}

/*
 * playlist_resolve_ex's index lookup: answer an entry from what the boot
 * pass already bound, with no directory read. It must give exactly the
 * walk's answer or none, so every step mirrors what fat32_resolve_path
 * would match (name_eq_ci on the long name) and declines anything it cannot
 * check:
 *
 *   - the path is exactly <lib root>/<album folder>/<file> ("Music/" only
 *     when the library lives under Music/); deeper or shallower paths walk.
 *   - the folder is one the index root walk mapped, matched by name_eq_ci on
 *     its stored name. The map cuts names to NAME_MAX - 1 bytes, so a
 *     component of that length could equal a longer name's cut: it, and
 *     anything longer, walks.
 *   - the file is a record BOUND at boot (file_clus set, so its dirent was
 *     seen in that folder), whose file_hash is the leaf's, whose stored
 *     on-disk stem equals the leaf's stem under name_eq_ci and is not cut
 *     at NAME_MAX, and whose format matches the leaf's extension. The hash
 *     covers the extension, which tells a ".fla" from a ".flac" of the same
 *     stem — the same hash-only test resolve_art_cb binds by.
 *
 * Short (8.3) names, unindexed files and the tag-scan fallback (no folder
 * map) all miss and are walked as before. The disk cannot change under a
 * running firmware, so the clusters bound at boot are still the files'.
 */
static int g_pl_index_hits;               /* this open's answered entries */

static int pl_index_lookup(void *ud, const char *path, playlist_track_t *t)
{
    (void)ud;
    if (g_folder_n == 0) return 0;
    char folder[NAME_MAX - 1], leaf[FAT32_NAME_BYTES];   /* folder: < NAME_MAX - 1 */
    const char *p = path;
    if (g_lib_root_clus) {
        char root[8];
        if (pl_path_part(&p, root, (int)sizeof root) < 0 ||
            !name_eq_ci(root, "Music")) return 0;
    }
    if (pl_path_part(&p, folder, (int)sizeof folder) < 0) return 0;
    if (pl_path_part(&p, leaf, (int)sizeof leaf) < 0 || *p) return 0;

    uint32_t dir = 0;
    uint32_t dh = name_hash(folder);
    for (int i = g_folder_hh[dh & (FOLDER_HASH_BUCKETS - 1)]; i; i = g_folder_hn[i - 1]) {
        if (g_folder_map[i - 1].hash == dh &&
            name_eq_ci(g_folder_map[i - 1].name, folder)) {
            dir = g_folder_map[i - 1].clus;
            break;
        }
    }
    if (dir == 0) return 0;

    int fmt = classify_ext(leaf);
    if (fmt < 0) return 0;
    char stem[NAME_MAX + 1];
    copy_display_name(stem, leaf, 1);
    int sl = 0;
    while (stem[sl]) sl++;
    if (sl >= NAME_MAX) return 0;          /* the stored stem may be cut */
    uint32_t fh = name_hash(leaf);
    if (fh == 0) return 0;
    for (int i = g_song_hh[fh & (SONG_HASH_BUCKETS - 1)]; i; i = g_song_hn[i - 1]) {
        const lib_song_t *s = &g_songs[i - 1];
        if (s->file_hash != fh || s->dir_clus != dir || s->file_clus == 0) continue;
        if (s->fmt != (uint8_t)fmt || !name_eq_ci(s->file, stem)) continue;
        int k = 0;
        for (; s->file[k]; k++) t->name[k] = s->file[k];
        t->name[k]   = '\0';
        t->clus      = s->file_clus;
        t->size      = s->file_size;
        t->dir_clus  = dir;
        t->file_hash = s->file_hash;
        t->fmt       = (uint8_t)fmt;
        g_pl_index_hits++;
        return 1;
    }
    return 0;
}

#ifdef MMIO_MOCK
/* Host builds only (the headless firmware): resolve the playlist again by
 * walking every entry and compare, row by row, with what the index answered.
 * The lookup's whole contract is "the walk's answer or none", and this is
 * where a real index on a real FAT image checks it. Never in the hw image:
 * it is the full walk the lookup exists to avoid, and a second 40 KB array. */
static playlist_track_t   g_pl_check[PLAYLIST_TRACKS_MAX];
static playlist_stats_t   g_pl_check_st;
static void pl_index_selfcheck(fat32_t *fs, int pi, int n)
{
    int w = playlist_resolve(fs, &g_playlists[pi], playlists_base_dir(),
                             g_pl_check, PLAYLIST_TRACKS_MAX,
                             &g_pl_scratch, &g_pl_check_st);
    uint32_t bad = (w != n) ? 1u : 0u;
    for (int i = 0; !bad && i < n; i++) {
        const playlist_track_t *a = &g_pl_tracks[i], *b = &g_pl_check[i];
        int k = 0;
        while (a->name[k] && a->name[k] == b->name[k]) k++;
        if (a->name[k] != b->name[k] || a->clus != b->clus ||
            a->size != b->size || a->dir_clus != b->dir_clus ||
            a->file_hash != b->file_hash || a->fmt != b->fmt) {
            bad = (uint32_t)i + 2u;          /* the first row that differs */
        }
    }
    uart_puts("core: playlist index check mismatch ");
    uart_put_hex32(bad);
    uart_putc('\n');
}
#endif

/* Open playlist `pi`: parse it, resolve every entry, bind the rows. Rows
 * that resolve to nothing are simply absent (counted in g_pl_stats); a
 * playlist file that cannot be read leaves g_pl_err set and no rows. */
void playlist_open(fat32_t *fs, int pi)
{
    g_list_epoch++;
    g_pl_open     = pi;
    g_pl_tracks_n = 0;
    g_pl_err      = 0;
    g_pl_slot     = 0;
    g_pl_damaged  = 0;
    g_plt_sel = g_plt_accum = 0;
    g_pl_confirm.armed = 0;
    if (pi < 0 || pi >= g_playlists_n) {
        return;
    }
    g_pl_index_hits = 0;
    int n = playlist_resolve_ex(fs, &g_playlists[pi], playlists_base_dir(),
                                g_pl_tracks, PLAYLIST_TRACKS_MAX,
                                &g_pl_scratch, &g_pl_stats,
                                pl_index_lookup, 0);
    /* Into the log: how much of the open the index answered. The rest were
     * directory walks, which is what to look at if a playlist opens slowly. */
    uart_puts("core: playlist rows ");
    uart_put_hex32((uint32_t)n);
    uart_puts(" index ");
    uart_put_hex32((uint32_t)g_pl_index_hits);
    uart_putc('\n');
#ifdef MMIO_MOCK
    if (n >= 0) pl_index_selfcheck(fs, pi, n);
#endif
    g_pl_err      = (n < 0) ? n : 0;
    g_pl_tracks_n = (n < 0) ? 0 : n;
    for (int i = 0; i < g_pl_tracks_n; i++) {
        g_pl_song[i] = (int16_t)playlist_bind_row(&g_pl_tracks[i]);
    }
    /*
     * A saved On-The-Go slot is an ordinary playlist in every way but two: it
     * gets a Delete Playlist row (without it, five saves dead-end the feature
     * until the next host sync), and it is checked for a TORN SAVE — a power
     * cut during the write leaves a header whose gen disagrees with the
     * trailer's, or a line count that disagrees with the header's. The parser
     * has just produced that count, so the check is two small reads and no
     * CRC pass. See library/otg_slot.h.
     *
     * otg_slot_of(), not otg_slot_index(): the NAME only says which slot this
     * COULD be, and a user is entitled to keep their own "On-The-Go 2.m3u8".
     * Offering Delete on that — a row that overwrites the file — is exactly
     * the data loss this feature is built to avoid, so the file has to carry
     * the directive before it is treated as ours. A playlist that would not
     * read is not ours either: g_pl_err leaves the row off.
     */
    g_pl_slot    = 0;
    g_pl_damaged = 0;
    if (g_pl_err == 0) {
        otg_slot_info_t info;
        g_pl_slot = otg_slot_of(fs, g_playlists[pi].name, g_playlists[pi].clus,
                                g_playlists[pi].size, g_pl_stats.listed, &info);
        if (g_pl_slot > 0) {
            g_pl_damaged = info.damaged ? 1 : 0;
        }
    }
}

/*
 * Play the open playlist from row `start`: the whole playlist is the queue,
 * in file order, each row carrying its own album's cover (a playlist mixes
 * albums, so there is no queue-level art — as Shuffle Songs). Returns the
 * queue index the pick landed on, -1 if nothing was queued. Does NOT
 * un-mute: the caller re-applies g_volume (see library_play_song).
 */
int playlist_play(int start)
{
    if (g_pl_open < 0 || g_pl_tracks_n == 0) {
        return -1;
    }
    if (start >= g_pl_tracks_n) {
        start = 0;                       /* the Delete row is not a track */
    }
    g_queue_kind     = RESUME_KIND_PLAYLIST;
    g_queue_seed     = 0;
    g_queue_ctx_hash = g_playlists[g_pl_open].hash;
    player_set_shuffle(g_settings.shuffle);
    player_queue_begin();
    for (int i = 0; i < g_pl_tracks_n; i++) {
        const playlist_track_t *t = &g_pl_tracks[i];
        browse_entry_t e;
        int k = 0;
        for (; t->name[k] && k < NAME_MAX; k++) e.name[k] = t->name[k];
        e.name[k]  = '\0';
        e.clus     = t->clus;
        e.size     = t->size;
        e.fmt      = t->fmt;
        e.is_dir   = 0;
        int ai = album_by_clus(t->dir_clus);
        e.art_clus = (ai >= 0) ? g_albums[ai].art_clus : 0;
        e.art_size = (ai >= 0) ? g_albums[ai].art_size : 0;
        /* Shuffle Albums grouping. A playlist row is not a library record —
         * only the bind (g_pl_song) knows its disc/track, so a row the index
         * never matched plays last within its album rather than pretending to
         * be its track 0. */
        e.album    = (ai >= 0) ? (uint16_t)(ai + 1) : 0;
        e.order_key = (g_pl_song[i] >= 0)
                        ? (((uint32_t)g_songs[g_pl_song[i]].disc << 16) |
                            g_songs[g_pl_song[i]].track)
                        : 0xFFFFFFFFu;
        player_queue_add(&e);
    }
    if (start < 0 || start >= g_pl_tracks_n) start = 0;
    player_queue_commit(start);
    return start;
}

/*
 * On-The-Go is PINNED as row 0, so the scanned files start at 1.
 *
 * The original iPod puts it last. First is chosen because 64 playlists is a
 * long spin to the bottom and this is the row that is used most — it is the
 * one the whole feature is reached through, and it is where a list you have
 * just built lives.
 */
int playlists_row_count(void) { return g_playlists_n + 1; }

void playlists_row_draw(int r, int idx)
{
    if (idx == 0) {
        char n[12];
        u32_to_dec(n, (unsigned)g_otg.n);
        list_row(r, "On-The-Go", 0, g_otg.n ? n : 0, 1 /*chevron*/,
                 idx == g_pl_sel, 0, 0);
        return;
    }
    list_row(r, g_playlists[idx - 1].name, 0, 0, 1 /*chevron*/,
             idx == g_pl_sel, 0, 0);
}

void playlists_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    int  total = playlists_row_count();
    char right[12];
    fmt_count(right, sel + 1, total);
    ui_header("Playlists", right, 1);
    if (g_playlists_err) {
        /* The pinned row is still real — On-The-Go lives in RAM and does not
         * need the folder — so the failure is a line under it, not the whole
         * screen. */
        playlists_row_draw(0, 0);
        ui_text(14, LIST_Y0 + ROW_H + 16, "Could not read Playlists",
                FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, total, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= total) break;
        playlists_row_draw(r, idx);
    }
    if (g_playlists_n == 0) {
        ui_text(14, LIST_Y0 + ROW_H + 16, "No playlist files",
                FONT_ROW, LINEN_MUTED);
        ui_text(14, LIST_Y0 + ROW_H + 36, "Put .m3u8 files in Music/Playlists",
                FONT_SMALL, LINEN_MUTED2);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, total);
}

/* One tracklist row: the Songs-list shape (tag title, artist sub-line,
 * duration on the right) for a row the library knows; the filename alone
 * for one it does not. */
/* A saved On-The-Go slot carries one extra row under its tracks. */
int playlist_row_count(void)
{
    return g_pl_tracks_n + (g_pl_slot > 0 ? 1 : 0);
}

void playlist_row_draw(int r, int idx)
{
    if (idx >= g_pl_tracks_n) {          /* the Delete row, saved slots only */
        const char *label = otg_confirm_up(&g_pl_confirm) ? "Delete? Select again"
                                                          : "Delete Playlist";
        ui_list_row(LIST_Y0, r, label, 0, 0, 0, idx == g_plt_sel, 0, 0, 1, ROW_H2);
        return;
    }
    const playlist_track_t *t = &g_pl_tracks[idx];
    const char *title = track_display(t->name), *sub = 0;
    char dur[FMT_TIME_MAX];
    dur[0] = '\0';
    int s = g_pl_song[idx];
    if (s >= 0) {
        const lib_song_t *sg = &g_songs[s];
        if (sg->title[0])   title = sg->title;
        if (sg->artist[0])  sub   = sg->artist;
        if (sg->duration_s) fmt_time(dur, sg->duration_s);
    }
    list_row_titled(r, title, sub, dur[0] ? dur : 0, idx == g_plt_sel, 0);
}

void playlist_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    int  total = playlist_row_count();
    char right[12];
    if (g_pl_tracks_n > 0 && sel < g_pl_tracks_n) {
        fmt_count(right, sel + 1, g_pl_tracks_n);
    } else {
        right[0] = '\0';
    }
    ui_header(g_pl_open >= 0 ? g_playlists[g_pl_open].name : "Playlist", right, 1);
    if (g_pl_tracks_n == 0) {
        /* Say which nothing this is: the file would not read, a save was torn
         * part-way through, it listed tracks none of which are on the disk, or
         * it lists none. */
        const char *why = g_pl_err            ? "Could not read playlist"
                        : g_pl_damaged        ? "Playlist damaged - save again"
                        : g_pl_stats.listed   ? "No tracks found on disk"
                        : g_pl_stats.rejected ? "Entries not usable"
                        :                       "Empty playlist";
        ui_text(14, LIST_Y0 + 20, why, FONT_ROW, LINEN_MUTED);
        if (g_pl_slot > 0) {
            /* The Delete row is the way out of a damaged or emptied slot, so
             * it survives the empty state. */
            playlist_row_draw(1, g_pl_tracks_n);
        }
        return;
    }
    int top = ui_scroll_window(sel, total, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= total) break;
        playlist_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, total);
}
