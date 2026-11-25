/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/library.c — see library.h.
 */

#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../kernel/resume_ctx.h"
#include "../ui/text.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "../library/idx.h"
#include "../library/sort.h"
#include "bootscreen.h"
#include "nav.h"
#include "main_internal.h"
#include "otg.h"
#include "library.h"

lib_album_t g_albums[LIB_MAX_ALBUMS];
int         g_albums_n;
uint16_t    g_albumview[LIB_MAX_ALBUMS];
int         g_albumview_n;

/* Set whenever a library load hits one of the fixed caps (LIB_MAX_SONGS /
 * LIB_MAX_ALBUMS / ARTISTS_MAX / LIB_MAX_GENRES / FOLDER_MAP_MAX), so the
 * truncation isn't silent — the About screen says the library didn't fit. */
int         g_lib_truncated;

/*
 * The other ways a load can come up short, kept SEPARATE from g_lib_truncated
 * because "Library too large" is the wrong thing to tell the user for any of
 * them — and until these existed, every one of them was silent: the tracks
 * were simply absent, or present and dead, with nothing to say why.
 *
 *   g_lib_unreadable  albums whose folder could not be read (disk error, or a
 *                     corrupt directory cluster) after the retries in
 *                     lib_readdir. Each such album has .unreadable set and all
 *                     its songs unresolved (file_clus == 0), so they list in
 *                     Songs/Genres but cannot play. Opening the album retries
 *                     the read and, on success, resolves it and decrements
 *                     this. UI: About should say "N albums could not be read";
 *                     the album's tracklist should say so instead of showing
 *                     an empty list (see g_browse_err).
 *   g_lib_orphaned    CORELIB.IDX records whose album folder is not on the
 *                     disk at all — a stale index, not a disk fault. Nothing
 *                     to retry; the fix is re-running the host importer. UI:
 *                     "index out of date, N tracks skipped".
 *   g_lib_load_err    nonzero (a FAT32_* code) when the library ROOT itself
 *                     could not be enumerated, or CORELIB.IDX could not be
 *                     READ (a disk error mid-stream, after the retries — as
 *                     opposed to an index that is corrupt, which falls back
 *                     to the scan), so there is no library at all this
 *                     session. The counts above are then meaningless
 *                     (0 songs). UI: a "could not read the disk" screen in
 *                     place of an empty Music menu; to retry, clear
 *                     g_lib_scanned and call library_ensure again.
 */
static int         g_lib_unreadable;
static int         g_lib_orphaned;
static int         g_lib_load_err;
uint32_t     g_lib_load_ms;   /* boot library load time, shown on About */

/* ---------------------------------------------------------------------------
 * Artists (menus.jsx ArtistsList): a list of the unique artist prefixes parsed
 * from the "Artist - Album" folder names. Selecting one filters the album list
 * (g_artist_filter) to just that artist's albums.
 * ------------------------------------------------------------------------- */
lib_artist_t g_artists[ARTISTS_MAX];
int          g_artists_n;

/* Build the unique, de-duplicated artist list from the index-derived album
 * list, then sort it A->Z. De-dup is by artist_key (leading "The " and case
 * ignored), so a differently-cased folder can't split one artist into two rows.
 * Because g_albums is index-driven, artists of stale on-disk folders never
 * appear here at all. */
static void build_artists(void)
{
    g_list_epoch++;
    g_artists_n = 0;
    for (int i = 0; i < g_albums_n; i++) {
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(g_albums[i].folder, artist, album);
        if (!artist[0]) continue;                 /* no "Artist - " prefix        */
        int found = 0;
        for (int j = 0; j < g_artists_n; j++) {
            if (title_cmp(artist_key(g_artists[j].name), artist_key(artist)) == 0) {
                found = 1;
                break;
            }
        }
        if (found) continue;
        if (g_artists_n >= ARTISTS_MAX) { g_lib_truncated = 1; continue; }
        lib_artist_t *a = &g_artists[g_artists_n];
        int k = 0;
        for (; artist[k] && k < NAME_MAX; k++) a->name[k] = artist[k];
        a->name[k]  = '\0';
        a->thm_clus = g_albums[i].thm_clus;   /* first album = artist's chip */
        a->thm_size = g_albums[i].thm_size;
        a->art_clus = g_albums[i].art_clus;
        a->art_size = g_albums[i].art_size;
        g_artists_n++;
    }

    /* Insertion-sort A->Z by artist_key (so "The xyz" files under X); the whole
     * struct moves, so each artist's chip travels with its name. */
    for (int i = 1; i < g_artists_n; i++) {
        lib_artist_t v = g_artists[i];
        int j = i - 1;
        while (j >= 0 && title_cmp(artist_key(g_artists[j].name),
                                   artist_key(v.name)) > 0) {
            g_artists[j + 1] = g_artists[j];
            j--;
        }
        g_artists[j + 1] = v;
    }
}

/* Build the on-screen album slice (indices into the already-sorted g_albums),
 * either all albums (artist_filter NULL/empty) or one artist's — matched by
 * artist_key so the "The "/case-insensitive grouping matches the Artists menu. */
void albumview_build(const char *artist_filter)
{
    g_list_epoch++;
    g_albumview_n = 0;
    for (int i = 0; i < g_albums_n && g_albumview_n < LIB_MAX_ALBUMS; i++) {
        if (artist_filter && artist_filter[0]) {
            char a[NAME_MAX + 1], b[NAME_MAX + 1];
            split_artist_album(g_albums[i].folder, a, b);
            if (title_cmp(artist_key(a), artist_key(artist_filter)) != 0) continue;
        }
        g_albumview[g_albumview_n++] = (uint16_t)i;
    }
}

/* ---------------------------------------------------------------------------
 * Library index (Songs / Genres): a one-time tag scan of every FLAC in the
 * library into RAM. The anti-skip buffer (~30 s) covers playback while the scan
 * hits the disk, so it can run without dropouts. Rebuilt only once per session.
 * ------------------------------------------------------------------------- */

lib_song_t g_songs[LIB_MAX_SONGS];
int        g_songs_n;
uint16_t   g_song_sorted[LIB_MAX_SONGS];   /* song indices, title order  */
char       g_genres[LIB_MAX_GENRES][LIB_GENRE_MAX];
int        g_genres_n;
int        g_genre_count[LIB_MAX_GENRES]; /* songs per genre (precomputed) */
static int        g_lib_scanned;

/* Root-folder name -> cluster map (built once), for resolving an index record's
 * album folder to a cluster without a per-album directory read. */
lib_folder_t g_folder_map[FOLDER_MAP_MAX];
int      g_folder_n;
/* folder_hash -> g_folder_map chains (index+1, 0 = end of chain), built by
 * folder_map_index() once the root walk is complete. folder_clus_h() used to
 * walk the whole map for EVERY record — on a full library ~3M hash compares
 * on the boot path, under the "Loading Library" bar. */
uint16_t g_folder_hh[FOLDER_HASH_BUCKETS];
uint16_t g_folder_hn[FOLDER_MAP_MAX];
static uint32_t g_idx_clus, g_idx_size;           /* CORELIB.IDX location        */

/* Scan temporaries (kept off the browser's g_browse). */
static uint32_t   g_scan_dirs[BROWSE_MAX];
static int        g_scan_dirs_n;
/* hash: name_hash over the FULL on-disk name, taken while the directory entry
 * is in hand — the same locator the index path gets from the host, so the
 * resolve pass binds scanned songs the same way and needs no name compare. */
typedef struct { char name[NAME_MAX + 1]; uint32_t clus, size, hash; uint8_t fmt; } scan_file_t;
static scan_file_t g_scan_files[BROWSE_MAX];
static int         g_scan_files_n;
static uint32_t    g_scan_art_clus, g_scan_art_size;

/* Filtered, on-screen song list (all songs, or one genre's). */
uint16_t   g_songview[LIB_MAX_SONGS];
int        g_songview_n;

static int16_t genre_intern(const char *g)
{
    if (!g[0]) return -1;
    for (int i = 0; i < g_genres_n; i++) {
        if (name_eq_ci(g_genres[i], g)) return (int16_t)i;
    }
    if (g_genres_n < LIB_MAX_GENRES) {
        int k = 0;
        for (; g[k] && k < LIB_GENRE_MAX - 1; k++) g_genres[g_genres_n][k] = g[k];
        g_genres[g_genres_n][k] = '\0';
        return (int16_t)g_genres_n++;
    }
    g_lib_truncated = 1;               /* out of genre slots */
    return -1;
}

/* ---------------------------------------------------------------------------
 * Lookup indexes: album by folder cluster, song by file hash
 *
 * Chained hash buckets, stored as index+1 so 0 means "end of chain". Both
 * used to be built once, AFTER the load, in library_finish — which fixed the
 * per-use lookups (the resolve pass, the queue builders) but left the load
 * itself linear: album_intern walked every album already listed to de-dupe
 * each record, and folder_clus_h walked the folder map for each one. On a
 * full library that is ~3M compares apiece, on an 80 MHz ARM7, while the user
 * watches "Loading Library". The album buckets are therefore maintained AS
 * albums are added (and rebuilt after the sort moves them); the song buckets
 * are built once the songs are all in, by lookup_build.
 * ------------------------------------------------------------------------- */

uint16_t g_song_hh[SONG_HASH_BUCKETS];
uint16_t g_song_hn[LIB_MAX_SONGS];
static uint16_t g_album_hh[ALBUM_HASH_BUCKETS];
static uint16_t g_album_hn[LIB_MAX_ALBUMS];

static void album_bucket_add(int i)
{
    uint32_t b = g_albums[i].clus & (ALBUM_HASH_BUCKETS - 1);
    g_album_hn[i] = g_album_hh[b];
    g_album_hh[b] = (uint16_t)(i + 1);
}

/* Album index by folder cluster, or -1. O(1): the chain for a cluster holds
 * ~2 albums on a full library. Valid at every moment — the buckets are reset
 * with the album list (albums_reset) and updated by album_intern — so the
 * loader can use it to de-dupe, not only the queue builders afterwards. */
int album_by_clus(uint32_t dc)
{
    for (int i = g_album_hh[dc & (ALBUM_HASH_BUCKETS - 1)]; i; i = g_album_hn[i - 1]) {
        if (g_albums[i - 1].clus == dc) return i - 1;
    }
    return -1;
}

/* Empty the album list. The ONLY way to: g_albums_n = 0 alone would leave the
 * buckets pointing at stale entries, and album_by_clus would keep answering
 * for albums that are no longer there. */
static void albums_reset(void)
{
    g_albums_n = 0;
    for (int i = 0; i < ALBUM_HASH_BUCKETS; i++) g_album_hh[i] = 0;
}

/* Add (folder, cluster) to the index-derived album list, de-duped by cluster. */
static void album_intern(const char *folder, uint32_t clus)
{
    if (clus == 0) return;
    if (album_by_clus(clus) >= 0) return;          /* already listed */
    if (g_albums_n >= LIB_MAX_ALBUMS) { g_lib_truncated = 1; return; }
    int k = 0;
    for (; folder[k] && k < NAME_MAX; k++) g_albums[g_albums_n].folder[k] = folder[k];
    g_albums[g_albums_n].folder[k] = '\0';
    g_albums[g_albums_n].clus = clus;
    g_albums[g_albums_n].unreadable = 0;     /* until the resolve pass says so */
    album_bucket_add(g_albums_n);
    g_albums_n++;
}

static int scan_dirs_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir || is_junk_dir(e->name)) return 0;
    if (g_scan_dirs_n < BROWSE_MAX) g_scan_dirs[g_scan_dirs_n++] = e->first_clus;
    album_intern(e->name, e->first_clus);          /* scan fallback album list */
    return 0;
}

static int scan_files_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_scan_art_clus = e->first_clus;
        g_scan_art_size = e->size;
        return 0;
    }
    int fmt = classify_ext(e->name);
    if (e->is_dir || fmt < 0) return 0;                    /* playable files */
    if (g_scan_files_n < BROWSE_MAX) {
        scan_file_t *f = &g_scan_files[g_scan_files_n++];
        copy_display_name(f->name, e->name, 1);
        f->clus = e->first_clus;
        f->size = e->size;
        f->hash = name_hash(e->name);          /* the locator: FULL name, with ext */
        f->fmt  = (uint8_t)fmt;                /* name[] is ext-trimmed: keep it   */
    }
    return 0;
}

/* Copy a fixed-length index field (NUL-terminated within it) to a bounded C
 * string, preserving UTF-8 (the atlas covers Latin-1 + smart punctuation); only
 * C0 control bytes become spaces. Byte-bounded — a split multibyte tail just
 * renders as one U+FFFD. */
static void field_copy(char *dst, int dcap, const uint8_t *src, int slen)
{
    int i = 0;
    for (; i < slen && i < dcap - 1 && src[i]; i++) {
        unsigned char c = src[i];
        dst[i] = (c >= 0x20) ? (char)c : ' ';   /* keep ASCII + all UTF-8 bytes */
    }
    dst[i] = '\0';
}

/* Root enumeration for the index path: capture CORELIB.IDX + a folder->cluster
 * map (so records resolve to a cluster with no per-album directory read). */
static int index_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir && name_eq_ci(e->name, "CORELIB.IDX")) {
        g_idx_clus = e->first_clus;
        g_idx_size = e->size;
        return 0;
    }
    if (e->is_dir && !is_junk_dir(e->name)) {
        if (g_folder_n >= FOLDER_MAP_MAX) { g_lib_truncated = 1; return 0; }
        copy_display_name(g_folder_map[g_folder_n].name, e->name, 0);
        /* The index's folder[] field is 64 bytes, so the host writer stores at
         * most 63 chars + NUL (build_index.py ascii_field). Cap the on-disk name
         * to the same 63 chars here, else a >63-char album folder never matches
         * (device kept 64, host kept 63) and all its tracks silently drop. */
        g_folder_map[g_folder_n].name[NAME_MAX - 1] = '\0';
        g_folder_map[g_folder_n].clus = e->first_clus;
        /* Locator hash over the FULL on-disk name (untruncated, folded), so it
         * matches the record's folder_hash even for >63-char folders. */
        g_folder_map[g_folder_n].hash = name_hash(e->name);
        g_folder_n++;
    }
    return 0;
}

/* Chain the folder map by hash. Built after the root walk rather than inside
 * index_root_cb because lib_readdir rewinds g_folder_n and re-runs the walk
 * on a retry, and a chain built during a walk that was then thrown away would
 * point at entries that no longer exist. */
static void folder_map_index(void)
{
    for (int i = 0; i < FOLDER_HASH_BUCKETS; i++) g_folder_hh[i] = 0;
    for (int i = 0; i < g_folder_n; i++) {
        uint32_t b = g_folder_map[i].hash & (FOLDER_HASH_BUCKETS - 1);
        g_folder_hn[i] = g_folder_hh[b];
        g_folder_hh[b] = (uint16_t)(i + 1);
    }
}

/* Resolve an index record's album folder to a cluster. Primary: match the
 * record's precomputed folder_hash (quote/case-folded) against the on-disk
 * folder hashes — one bucket, a chain of ~1. Two root folders that differ
 * only in what the hash folds share it; as in resolve_art_cb the record whose
 * stored folder name is the on-disk name byte for byte wins, else the first.
 * (The record's folder[] is the host's display form, which straightens a
 * curly apostrophe in the tag album, so a "It's" / "It’s" folder pair can
 * still both read as "It's" — then this is no worse than before.) Fallback:
 * the legacy case-insensitive name compare, so a hash mismatch can never
 * regress below the old behaviour; it is linear, but only an orphaned record
 * (or a fold disagreement the parity test exists to prevent) gets that far. */
uint32_t folder_clus_h(uint32_t hash, const char *name)
{
    int pick = 0, pick_checked = 0;
    for (int i = g_folder_hh[hash & (FOLDER_HASH_BUCKETS - 1)]; i; i = g_folder_hn[i - 1]) {
        if (g_folder_map[i - 1].hash != hash) continue;
        if (!pick) { pick = i; continue; }
        if (!pick_checked) {
            pick_checked = 1;
            if (name_bind_exact(name, g_folder_map[pick - 1].name, 0)) break;
        }
        if (name_bind_exact(name, g_folder_map[i - 1].name, 0)) { pick = i; break; }
    }
    if (pick) return g_folder_map[pick - 1].clus;
    for (int i = 0; i < g_folder_n; i++) {
        if (name_eq_ci(g_folder_map[i].name, name)) return g_folder_map[i].clus;
    }
    return 0;
}

/* Songs are keyed by file_hash — the folded hash of the FULL on-disk
 * filename, extension included, which is the one form both sides have
 * exactly: the host stamped it from the name it gave the file, the resolve
 * pass hashes the directory entry. (They used to be keyed by a hash of the
 * ext-trimmed file[] field, which for a long name is a hash of a truncated
 * string that nothing on the disk produces — the bucket missed and a linear
 * sweep quietly made up the difference.) Albums: rebuilt here because the
 * album sort in library_finish has just moved them. */
static void lookup_build(void)
{
    for (int i = 0; i < SONG_HASH_BUCKETS; i++)  g_song_hh[i]  = 0;
    for (int i = 0; i < ALBUM_HASH_BUCKETS; i++) g_album_hh[i] = 0;
    for (int i = 0; i < g_songs_n; i++) {
        uint32_t b = g_songs[i].file_hash & (SONG_HASH_BUCKETS - 1);
        g_song_hn[i] = g_song_hh[b];
        g_song_hh[b] = (uint16_t)(i + 1);
    }
    for (int i = 0; i < g_albums_n; i++) album_bucket_add(i);
}

/* ---------------------------------------------------------------------------
 * Directory reads the library depends on.
 *
 * Every fat32_readdir in the loader used to ignore its return code. The walk
 * can fail — FAT32_EIO when a sector read fails even after player_disk_read's
 * six attempts, FAT32_ECORRUPT for a cyclic or unaddressable chain — and not
 * one caller looked. The consequence was never a crash, which is why it went
 * unnoticed: a failed album walk in the resolve pass just left every song in
 * that album with file_clus == 0, and every later path reads that as "indexed
 * but no longer on disk". The tracks stayed listed in Songs and Genres and
 * did nothing when picked, until reboot; opening the album showed an empty
 * tracklist. The trigger is mundane — the drive still settling from spin-up
 * during "Loading Library" — so this is a bug users hit, not a hypothetical.
 *
 * lib_readdir is fat32_readdir with the two things the loader needs:
 *
 *   1. A RETRY. player_disk_read already retries each sector six times with
 *      short backoffs, so a walk that failed has already burned ~200 ms on
 *      the failing sector; a further LIB_READDIR_RETRY_MS pause and a fresh
 *      walk gives a drive that was mid-settle its second chance. Only an EIO
 *      is retried: ECORRUPT is structural (the bytes are wrong, not late) and
 *      re-walking a cyclic chain costs the full bounded scan again for
 *      nothing. The retry re-runs the callback from the top of the directory,
 *      so the caller's accumulator is REWOUND first (`acc_n`) — otherwise the
 *      entries seen before the failure would be listed twice.
 *
 *   2. A BUDGET. A dead drive fails every read; with a thousand albums to
 *      resolve, retrying each one would turn a failed boot into minutes of
 *      sleeping before the user even sees an error. g_lib_retry_budget is the
 *      number of retries a whole load may spend: a spin-up is one event and a
 *      couple of retries ride it out, so if the budget is gone and reads are
 *      still failing, this is not spin-up and the remaining albums are marked
 *      unreadable at the cost of one plain (already-retried) walk each.
 *
 * The common case — the walk succeeds first time — pays one compare. The
 * loader is not slower for it.
 *
 * On a final failure the accumulator is rewound too: a half-listed directory
 * is not "the files in this album", and presenting it as such is a subtler
 * version of the bug this replaces. The return is the last FAT32_* code.
 * ------------------------------------------------------------------------- */
#define LIB_READDIR_RETRY_MS  250    /* settle time before each re-walk           */
#define LIB_LOAD_RETRY_BUDGET 6      /* re-walks a whole library load may spend   */

int g_lib_retry_budget;

int lib_readdir(fat32_t *fs, uint32_t clus, fat32_dir_cb cb, void *ud,
                       int *acc_n)
{
    int base = acc_n ? *acc_n : 0;
    int rc   = fat32_readdir(fs, clus, cb, ud);
    for (int attempt = 0;
         rc == FAT32_EIO && attempt < LIB_READDIR_RETRIES && g_lib_retry_budget > 0;
         attempt++) {
        g_lib_retry_budget--;
        sleep_ms(LIB_READDIR_RETRY_MS);
        if (acc_n) *acc_n = base;
        rc = fat32_readdir(fs, clus, cb, ud);
    }
    if (rc != 0 && acc_n) *acc_n = base;
    return rc;
}

/* Resolve pass: ONE readdir per album at load time captures both the folder's
 * cover clusters (folder.art/.thm) AND every track file's own cluster into
 * g_songs — so later cover loads, playing a song, and shuffling the WHOLE
 * library are all direct reads with no per-use directory scan. */
static uint32_t g_res_art_clus, g_res_art_size, g_res_thm_clus, g_res_thm_size;
static uint32_t g_res_album_clus;
static int resolve_art_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir) return 0;
    if (name_eq_ci(e->name, "folder.thm")) {
        g_res_thm_clus = e->first_clus; g_res_thm_size = e->size; return 0;
    }
    if (name_eq_ci(e->name, "folder.art")) {
        g_res_art_clus = e->first_clus; g_res_art_size = e->size; return 0;
    }
    if (classify_ext(e->name) < 0) return 0;      /* a playable track: bind its cluster */
    /* The locator, and the only test: the folded hash of the FULL on-disk name
     * equals the record's file_hash (stamped by build_index.py over the name it
     * gave the file; computed at readdir for a scanned song), within this
     * album, for a record not yet bound. No name compare and no fallback — a
     * record that hashes to no file in its folder is not on the disk. The old
     * "hash didn't line up" sweep over all songs existed to rescue the long
     * names whose truncated file[] could not match the bucket key; that was
     * ~6000 compares per such file at every boot, and it is what let the
     * truncation stay invisible for as long as it did. */
    uint32_t fh = name_hash(e->name);
    if (fh == 0) return 0;                        /* 0 is "no locator", never a match */
    /* Almost always exactly one candidate, and it binds without a byte of
     * name compared. Two or more is a pair of names in one folder that differ
     * only in what name_hash folds — "It's" / "It’s", case — and the first in
     * chain order used to win, so the pair bound in directory order and could
     * swap titles and durations. The tiebreak is name_bind_exact: the record
     * whose stored stem IS the on-disk stem, byte for byte. A pair whose stems
     * both outgrew the 63-byte field can match neither way and keeps the old
     * order (the index has nothing else the dirent could be checked against:
     * it carries a duration, the dirent a size). */
    lib_song_t *pick = 0;
    int pick_checked = 0;
    for (int i = g_song_hh[fh & (SONG_HASH_BUCKETS - 1)]; i; i = g_song_hn[i - 1]) {
        lib_song_t *s = &g_songs[i - 1];
        if (s->file_clus || s->dir_clus != g_res_album_clus || s->file_hash != fh) continue;
        if (!pick) { pick = s; continue; }
        if (!pick_checked) {
            pick_checked = 1;
            if (name_bind_exact(pick->file, e->name, 1)) break;
        }
        if (name_bind_exact(s->file, e->name, 1)) { pick = s; break; }
    }
    if (!pick) return 0;
    pick->file_clus = e->first_clus;
    pick->file_size = e->size;
    /* The record's stored name has no extension, so the DIRENT is the only
     * place the format is knowable — and it is in hand exactly here. */
    pick->fmt = (uint8_t)classify_ext(e->name);
    /* Bound. From here on the song is shown and located by its ON-DISK name:
     * the stem, capped exactly as a browse row is (same function, same
     * NAME_MAX), so the queue entry, the tracklist row and this field are
     * the same bytes — and its hash is what resume_capture will store. */
    copy_display_name(pick->file, e->name, 1);
    pick->stem_hash = name_hash(pick->file);
    return 0;
}
/*
 * Resolve ONE album: read its folder, bind its songs' clusters and capture its
 * art. Returns the readdir result. This is the one place an album is marked
 * unreadable or cleared again, so the count in g_lib_unreadable can never
 * drift from the flags — the load-time pass and a later browse_load both come
 * through here.
 *
 * On failure the art fields are left cleared (there is nothing to show) and
 * songs that happened to be bound before the walk failed KEEP their binding:
 * those entries were read correctly, and unbinding a playable track because a
 * sibling's sector was bad would be manufacturing a second failure.
 */
int album_resolve(fat32_t *fs, int i)
{
    g_res_art_clus = g_res_art_size = g_res_thm_clus = g_res_thm_size = 0;
    g_res_album_clus = g_albums[i].clus;
    int rc = lib_readdir(fs, g_albums[i].clus, resolve_art_cb, 0, 0);
    if (rc != 0) {
        if (!g_albums[i].unreadable) {
            g_albums[i].unreadable = 1;
            g_lib_unreadable++;
        }
        g_albums[i].art_clus = g_albums[i].art_size = 0;
        g_albums[i].thm_clus = g_albums[i].thm_size = 0;
        return rc;
    }
    if (g_albums[i].unreadable) {
        g_albums[i].unreadable = 0;
        g_lib_unreadable--;
    }
    g_albums[i].art_clus = g_res_art_clus;
    g_albums[i].art_size = g_res_art_size;
    g_albums[i].thm_clus = g_res_thm_clus;
    g_albums[i].thm_size = g_res_thm_size;
    return 0;
}

static void library_resolve_art(fat32_t *fs)
{
    for (int s = 0; s < g_songs_n; s++) g_songs[s].file_clus = 0;
    for (int a = 0; a < g_albums_n; a++) g_albums[a].unreadable = 0;
    g_lib_unreadable = 0;

    /*
     * Walk the albums in ASCENDING CLUSTER order, not menu order.
     *
     * This pass reads one directory per album — up to LIB_MAX_ALBUMS of them —
     * and each read is a seek to wherever that folder's directory cluster
     * happens to live. Album (alphabetical) order bears no relation to on-disk
     * order, so the head was being thrown back and forth across an 80 GB platter
     * a couple of hundred times, and seek time, not transfer time, dominated
     * "Loading Library". Sorted, the same reads become a single forward sweep,
     * which is also the order the drive's own readahead can help with.
     *
     * Results are written back through the original index, so nothing outside
     * this function sees a different order.
     */
    static uint16_t order[LIB_MAX_ALBUMS];
    int n = g_albums_n;
    for (int i = 0; i < n; i++) order[i] = (uint16_t)i;
    for (int i = 1; i < n; i++) {             /* insertion sort: n <= 256 */
        uint16_t v = order[i];
        uint32_t k = g_albums[v].clus;
        int j = i - 1;
        while (j >= 0 && g_albums[order[j]].clus > k) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }

    for (int p = 0; p < n; p++) {
        int i = order[p];
        /* load_bar is time-throttled, so calling it per album is free and the
         * bar advances smoothly instead of in 4-album jumps. */
        load_bar("LOADING LIBRARY", 75 + (n ? p * 25 / n : 25));
        (void)album_resolve(fs, i);    /* failure is recorded on the album */
    }
}

/* Why the last index load was refused (an IDX_* code), for the About screen
 * and the UART. A refused index is not silent: the device falls back to the
 * tag scan, which takes minutes, and the user deserves to know it was the
 * file and not the disk. */
static int g_lib_idx_reject;

/* Refuse the index: log why, discard whatever was parsed before the check
 * failed (a CRC is only known at the end, by which time the records are in
 * g_songs), and return the loader's "fall back to a scan" result. */
static int idx_reject(int why)
{
    static const char *const names[] = {
        "ok", "magic", "version", "recsize", "size", "crc", "read"
    };
    g_lib_idx_reject = why;
    uart_puts("idx: rejected (");
    uart_puts(names[why]);
    uart_puts(")\n");
    g_songs_n = g_genres_n = 0;
    albums_reset();
    g_lib_orphaned = 0;
    return 0;
}

/* Load the whole library from the host-built CORELIB.IDX in ONE streamed pass
 * (no per-file tag reads) — instant Songs/Genres/durations/disc. Returns 1 on
 * success, 0 if the index is absent/bad (caller falls back to a scan).
 * Record (256B, LE): u32 dur, u16 track, u16 disc, folder[64], file[64],
 * title[48], artist[40], genre[24], u32 folder_hash, u32 file_hash. */
static void library_finish(void);        /* sort + genre counts (shared)        */

/* The library root: the "Music" folder if present, else the volume root (kept
 * for back-compat). Music/ holds the album folders + CORELIB.IDX, so the FAT
 * root stays clean (core.ipod / loader.cfg / Apple system folders only). */
uint32_t g_lib_root_clus;
static int lib_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir && name_eq_ci(e->name, "Music")) {
        g_lib_root_clus = e->first_clus;
    }
    return 0;
}
static uint32_t lib_root(fat32_t *fs)
{
    g_lib_root_clus = 0;
    /* If the volume root cannot be read there is no finding Music/, and the
     * fallback to the root cluster below will fail the same way one call later.
     * Record it so the load reports "could not read the disk" rather than
     * quietly producing a library of zero songs. */
    int rc = lib_readdir(fs, fs->root_clus, lib_root_cb, 0, 0);
    if (rc != 0) g_lib_load_err = rc;
    return g_lib_root_clus ? g_lib_root_clus : fs->root_clus;
}

static int library_load_index(fat32_t *fs)
{
    g_idx_clus = 0;
    g_folder_n = 0;
    /* This one walk is the whole folder map: if it fails, every index record
     * resolves to "album not on disk" and the library silently loads EMPTY —
     * the whole-library version of the per-album bug. Rewinding g_folder_n
     * on failure (lib_readdir does) matters here for the same reason: a
     * half-built map would resolve half the library and orphan the rest. */
    int rc = lib_readdir(fs, lib_root(fs), index_root_cb, 0, &g_folder_n);
    if (rc != 0) {
        g_lib_load_err = rc;
        return 0;
    }
    if (g_idx_clus == 0) return 0;
    folder_map_index();                    /* the walk is final: chain it     */
    g_lib_idx_reject = IDX_OK;

    fat32_stream_t st;
    fat32_stream_open(&st, fs, g_idx_clus, g_idx_size);
    uint8_t hdr[IDX_HDR_V2];
    if (fat32_stream_read(&st, hdr, IDX_HDR_V1) != (int32_t)IDX_HDR_V1) {
        return idx_reject(IDX_EREAD);
    }
    /* A v2 header is four bytes longer; on a v1 file those bytes are record 0
     * and the stream cannot rewind, so read them only once the version says
     * they are there. idx_header_parse validates the version. */
    if ((hdr[4] | (hdr[5] << 8)) == 2 &&
        fat32_stream_read(&st, hdr + IDX_HDR_V1, IDX_HDR_V2 - IDX_HDR_V1)
            != (int32_t)(IDX_HDR_V2 - IDX_HDR_V1)) {
        return idx_reject(IDX_EREAD);
    }
    idx_hdr_t h;
    int hrc = idx_header_parse(hdr, g_idx_size, &h);
    if (hrc != IDX_OK) return idx_reject(hrc);
    uint32_t count = h.count;
    uint32_t crc   = 0xFFFFFFFFu;

    g_songs_n = g_genres_n = 0;
    albums_reset();
    g_lib_truncated = 0;
    /* Read the index in 16 KB batches (64 records) rather than 256 B at a time:
     * a 256 B stream read pulls a whole 2048 B FS-sector and hands back 256 B, so
     * per-record reads re-fetched each sector 8x. Batching is ~14 big reads for
     * the whole index instead of ~900 tiny ones — the bulk of the load time. */
    static uint8_t idxbuf[64 * 256];
    uint32_t n = 0;
    while (n < count && g_songs_n < LIB_MAX_SONGS) {
        uint32_t batch = count - n;
        if (batch > 64) batch = 64;
        int32_t got = fat32_stream_read(&st, idxbuf, batch * 256u);
        /*
         * A failed read is NOT the end of the index. `if (got <= 0) break;`
         * used to stand here, so an EIO or ECORRUPT mid-file ended the loop
         * quietly: n < count then flagged the library "too large", the CRC
         * was skipped (it only runs when every record streamed past), and
         * library_finish ran on the partial set — half the library missing,
         * no scan fallback, no retry, and About blaming the caps for it.
         *
         * EIO is the drive, not the file: retried on the load's budget
         * (fat32_stream_read leaves the cursor untouched on an error, so the
         * same call is simply repeated), and if it still fails the load is
         * refused with g_lib_load_err set — "could not read the disk", no
         * scan fallback against a disk that just failed, and a deliberate
         * retry clears g_lib_scanned. Anything else — ECORRUPT, or a read
         * that came up short of the batch the header's size promised — is
         * the FILE: rejected like a CRC mismatch, so the tag scan takes over.
         */
        for (int attempt = 0;
             got == FAT32_EIO && attempt < LIB_READDIR_RETRIES && g_lib_retry_budget > 0;
             attempt++) {
            g_lib_retry_budget--;
            sleep_ms(LIB_READDIR_RETRY_MS);
            got = fat32_stream_read(&st, idxbuf, batch * 256u);
        }
        if (got == FAT32_EIO) {
            g_lib_load_err = FAT32_EIO;
            return idx_reject(IDX_EREAD);
        }
        if (got < 0 || (uint32_t)got < batch * 256u) {
            return idx_reject(IDX_EREAD);
        }
        uint32_t recs = batch;
        if (h.has_crc) crc = crc32_update(crc, idxbuf, recs * 256u);
        for (uint32_t k = 0; k < recs && g_songs_n < LIB_MAX_SONGS; k++) {
            const uint8_t *r = idxbuf + k * 256u;
            char folder[NAME_MAX + 1];
            field_copy(folder, sizeof folder, r + 8, 64);
            uint32_t folder_hash = (uint32_t)r[248] | ((uint32_t)r[249] << 8) |
                                   ((uint32_t)r[250] << 16) | ((uint32_t)r[251] << 24);
            uint32_t file_hash   = (uint32_t)r[252] | ((uint32_t)r[253] << 8) |
                                   ((uint32_t)r[254] << 16) | ((uint32_t)r[255] << 24);
            uint32_t dc = folder_clus_h(folder_hash, folder);
            if (dc == 0) {
                /* The index names an album folder the disk does not have: a
                 * stale CORELIB.IDX (album deleted, importer not re-run), not
                 * a read fault — the folder map above was read in full. The
                 * record is dropped, but counted, so the user can be told the
                 * index is out of date rather than wondering where it went. */
                g_lib_orphaned++;
                continue;
            }
            lib_song_t *s = &g_songs[g_songs_n];
            s->dir_clus   = dc;
            s->file_hash  = file_hash;
            s->duration_s = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
                            ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
            s->track = (uint16_t)(r[4] | (r[5] << 8));
            s->disc  = (uint16_t)(r[6] | (r[7] << 8));
            field_copy(s->title,  LIB_TITLE_MAX,  r + 136, 48);
            field_copy(s->artist, LIB_ARTIST_MAX, r + 184, 40);
            field_copy(s->file,   LIB_FILE_MAX,   r + 72,  64);
            trim_audio_ext(s->file);           /* display placeholder, see there */
            /* Provisional resume locator, replaced by the on-disk stem's hash
             * when the record binds. Exact for a name that fit the field. For
             * one that did not it is the hash of a truncated string no
             * directory entry produces — so it can never resume the wrong
             * file, and it still counts as a same-named twin in
             * resume_find_song's ambiguity rule if the record never binds
             * (an unreadable album), which is the conservative side. */
            s->stem_hash = name_hash(s->file);
            char genre[LIB_GENRE_MAX];
            field_copy(genre, LIB_GENRE_MAX, r + 224, 24);
            s->genre = genre_intern(genre);
            g_songs_n++;
            album_intern(folder, dc);          /* album has >=1 indexed song */
        }
        n += recs;
        load_bar("LOADING LIBRARY",            /* first ~75% = reading the index */
                 count ? (int)(n * 75u / count) : 0);
    }
    if (n < count) g_lib_truncated = 1;    /* ran out of song slots: the only
                                            * way out of the loop early now  */
    /* The CRC is over ALL the records, so it can only be checked when all of
     * them streamed past. A load cut short by LIB_MAX_SONGS has not read them
     * all — it is already flagged truncated, and the host refuses to write an
     * index over the cap, so this is the one case that rides on the size
     * check alone rather than reading the rest of the file for nothing. */
    if (h.has_crc && n == count && ~crc != h.crc) return idx_reject(IDX_ECRC);
    library_finish();
    library_resolve_art(fs);               /* index each album's cover clusters */
    g_lib_scanned = 1;
    return 1;
}

/* Walk the library root (Music/ or the volume root) → album folders → FLAC
 * files, probing each file's tags. */
static void library_scan(fat32_t *fs)
{
    if (g_lib_scanned) return;
    g_songs_n = g_genres_n = g_scan_dirs_n = 0;
    albums_reset();
    g_lib_truncated = 0;
    /* album_intern de-dupes by cluster, so the retry's re-walk of the folder
     * list is idempotent for g_albums; only g_scan_dirs needs the rewind. */
    int rc = lib_readdir(fs, lib_root(fs), scan_dirs_cb, 0, &g_scan_dirs_n);
    if (rc != 0) g_lib_load_err = rc;
    if (g_scan_dirs_n >= BROWSE_MAX) g_lib_truncated = 1;   /* folder list full */
    for (int d = 0; d < g_scan_dirs_n && g_songs_n < LIB_MAX_SONGS; d++) {
        g_scan_files_n = 0;
        g_scan_art_clus = g_scan_art_size = 0;
        /* A failed file walk leaves this album with no songs in the scan; the
         * resolve pass below reads the folder again and is where it gets
         * marked unreadable (or not, if the disk has settled by then — in
         * which case the album browses fine but its tracks are missing from
         * Songs until the next boot; the index path, which ships, has no such
         * gap because its song list does not come from this walk). */
        (void)lib_readdir(fs, g_scan_dirs[d], scan_files_cb, 0, &g_scan_files_n);
        for (int i = 0; i < g_scan_files_n && g_songs_n < LIB_MAX_SONGS; i++) {
            player_pump();                 /* keep audio fed during the scan     */
            if ((g_songs_n & 31) == 0) {   /* live progress every 32 tracks       */
                console_clear(LINEN_SURFACE);
                ui_text_centered(112, "Building Library", FONT_TITLE, LINEN_INK);
                char pg[16];
                int pl = u32_to_dec(pg, (unsigned)g_songs_n);
                for (const char *p = " songs"; *p; p++) pg[pl++] = *p;
                pg[pl] = '\0';
                ui_text_centered(134, pg, FONT_SUB, LINEN_MUTED);
                lcd_present_fb(console_framebuffer());
            }
            flac_meta_t m;
            int ok = (player_probe_meta(g_scan_files[i].clus,
                                        g_scan_files[i].size, &m) == 0);
            lib_song_t *s = &g_songs[g_songs_n++];
            const char *t = (ok && m.have && m.title[0]) ? m.title
                                                         : g_scan_files[i].name;
            field_copy(s->title, LIB_TITLE_MAX, (const uint8_t *)t,
                       (int)sizeof s->title);
            if (ok && m.have && m.artist[0])
                field_copy(s->artist, LIB_ARTIST_MAX, (const uint8_t *)m.artist, 64);
            else
                s->artist[0] = '\0';
            field_copy(s->file, LIB_FILE_MAX,
                       (const uint8_t *)g_scan_files[i].name, NAME_MAX);
            s->file_hash  = g_scan_files[i].hash;  /* binds like an index record */
            s->stem_hash  = name_hash(s->file);    /* exact: this IS the disk stem */
            s->dir_clus   = g_scan_dirs[d];
            s->duration_s = (ok && m.have) ? m.duration_s : 0;
            s->track      = (ok && m.have) ? (uint16_t)m.track : 0;
            s->disc       = 0;
            s->genre      = (ok && m.have) ? genre_intern(m.genre) : -1;
            s->fmt        = g_scan_files[i].fmt;
        }
    }
    if (g_songs_n >= LIB_MAX_SONGS) g_lib_truncated = 1;   /* out of song slots */
    library_finish();
    library_resolve_art(fs);               /* index each album's cover clusters */
    g_lib_scanned = 1;
}

/* Scratch for merge_sort_idx. Sized to the larger of the two things sorted. */
uint16_t g_sort_tmp[LIB_MAX_SONGS];

/* Album sort keys, parsed ONCE. split_artist_album is a string scan, and the
 * old insertion sort re-ran it twice per comparison. */
char g_album_key[LIB_MAX_ALBUMS][NAME_MAX + 1];

static int song_title_cmp_idx(uint16_t a, uint16_t b)
{
    return title_cmp(g_songs[a].title, g_songs[b].title);
}

static int album_key_cmp_idx(uint16_t a, uint16_t b)
{
    return title_cmp(g_album_key[a], g_album_key[b]);
}

static int genre_cmp_idx(uint16_t a, uint16_t b)
{
    return title_cmp(g_genres[a], g_genres[b]);
}

/* The genre sort's order array. Its own, not g_sort_tmp: that is the scratch
 * merge_sort_idx needs, and the inverse permutation is built into it. */
static uint16_t g_genre_order[LIB_MAX_GENRES];

/*
 * Alphabetise the genre table A->Z.
 *
 * genre_intern() appends in FIRST-SEEN order, which is the order the index
 * records happen to arrive in, so Music > Genres was the one long list on the
 * device with no order at all to read down or to locate within. Sorting it is
 * also what lets the A-Z plate appear there: the letter index rejects a list
 * whose initial changes on nearly every row.
 *
 * A genre is identified by its INDEX (lib_song_t.genre, songview_build's
 * argument), so the sort has to remap every song's field through the inverse
 * permutation. That is safe here and nowhere else: library_finish is the last
 * thing the load does, before any screen or the resume restore can capture an
 * index. Nothing on disk holds one — the resume record stores the song, and
 * rebuilds the genre view from the song's own field.
 */
static void genres_sort(void)
{
    int n = g_genres_n;
    if (n <= 1) {
        return;
    }
    for (int i = 0; i < n; i++) g_genre_order[i] = (uint16_t)i;
    merge_sort_idx(g_genre_order, n, g_sort_tmp, genre_cmp_idx);

    /* inv[old] = new: both the remap the song fields need and the SCATTER
     * permutation the in-place apply below follows — the sort hands back a
     * gather order, exactly as in the album sort above. g_sort_tmp is free
     * again now the sort is done, and the apply consumes it. */
    uint16_t *inv = g_sort_tmp;
    for (int k = 0; k < n; k++) inv[g_genre_order[k]] = (uint16_t)k;
    for (int i = 0; i < g_songs_n; i++) {
        int g = g_songs[i].genre;
        if (g >= 0 && g < n) g_songs[i].genre = (int16_t)inv[g];
    }
    for (int i = 0; i < n; i++) {
        while (inv[i] != (uint16_t)i) {
            int j = inv[i];
            for (int k = 0; k < LIB_GENRE_MAX; k++) {
                char ck = g_genres[i][k];
                g_genres[i][k] = g_genres[j][k];
                g_genres[j][k] = ck;
            }
            uint16_t tk = inv[i];
            inv[i] = inv[j];
            inv[j] = tk;
        }
    }
}

/* Shared post-load: title-sort the index array + precompute per-genre counts. */
static void library_finish(void)
{
    for (int i = 0; i < g_songs_n; i++) g_song_sorted[i] = (uint16_t)i;
    merge_sort_idx(g_song_sorted, g_songs_n, g_sort_tmp, song_title_cmp_idx);
    /* Before the counts: the sort renumbers the genres the counts are indexed
     * by, so counting first would mean permuting g_genre_count[] as well. */
    genres_sort();
    for (int i = 0; i < g_genres_n; i++) g_genre_count[i] = 0;
    for (int i = 0; i < g_songs_n; i++) {
        int g = g_songs[i].genre;
        if (g >= 0 && g < g_genres_n) g_genre_count[g]++;
    }

    /* Alphabetise the album list A->Z by album title (insertion sort; the list
     * is small). g_albumview is derived from this, so the menu is sorted too.
     * The lookup indexes are (re)built AFTER this, since the sort moves albums. */
    {
        int n = g_albums_n;
        for (int i = 0; i < n; i++) {
            char artist[NAME_MAX + 1];
            split_artist_album(g_albums[i].folder, artist, g_album_key[i]);
            g_albumview[i] = (uint16_t)i;          /* reused as the order array */
        }
        merge_sort_idx(g_albumview, n, g_sort_tmp, album_key_cmp_idx);

        /*
         * Apply the permutation in place by following cycles, so we never need
         * a second full lib_album_t array (85 B x LIB_MAX_ALBUMS). g_albumview
         * is rebuilt by the caller right after this.
         *
         * The swap loop SCATTERS (dst[P[i]] = src[i]); what the sort gives us
         * is a GATHER order (dst[k] = src[order[k]]). So invert first — with
         * the raw order it silently produces the inverse permutation, i.e. a
         * scrambled album list that is still a valid permutation and therefore
         * looks plausible. Verified against a reference implementation over 200
         * randomised trials (order, stability, and this in-place apply).
         */
        for (int k = 0; k < n; k++) g_sort_tmp[g_albumview[k]] = (uint16_t)k;
        for (int i = 0; i < n; i++) {
            while (g_sort_tmp[i] != (uint16_t)i) {
                int j = g_sort_tmp[i];
                lib_album_t t = g_albums[i];
                g_albums[i] = g_albums[j];
                g_albums[j] = t;
                /* The KEY travels with its album. It did not, and nothing
                 * noticed because after this loop g_album_key[] was never read
                 * again — it existed only to make the comparator cheap. The
                 * A-Z locator reads it (the album initial is the initial of
                 * the album title, and re-splitting the folder name per row
                 * under a spinning wheel is exactly the string scan this array
                 * was introduced to stop), so a stale key is now a wrong
                 * letter on the plate rather than a dead array. */
                for (int k = 0; k <= NAME_MAX; k++) {
                    char ck = g_album_key[i][k];
                    g_album_key[i][k] = g_album_key[j][k];
                    g_album_key[j][k] = ck;
                }
                uint16_t tk = g_sort_tmp[i];
                g_sort_tmp[i] = g_sort_tmp[j];
                g_sort_tmp[j] = tk;
            }
        }
    }

    lookup_build();                        /* song-by-name + album-by-cluster */
}

/* Show a "building library" splash then scan (blocks; anti-skip covers audio). */
void library_ensure(fat32_t *fs)
{
    if (g_lib_scanned) return;
    /* Time the whole load and surface it on About. This is the one long wait at
     * boot and it is dominated by disk seeks, so it is worth being able to see
     * whether a change actually helped instead of judging it by feel. */
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    load_bar("LOADING LIBRARY", 0);       /* the load phases fill this in */
    g_lib_load_err     = 0;
    g_lib_orphaned     = 0;
    g_lib_unreadable   = 0;
    g_lib_retry_budget = LIB_LOAD_RETRY_BUDGET;
    if (!library_load_index(fs) &&        /* host-built CORELIB.IDX */
        !g_lib_load_err)                  /* ...absent, not unreadable: */
        library_scan(fs);                  /* fallback: per-file tag scan     */
    /* A root that could not be read leaves neither path having run to the
     * end. Mark the load done anyway: library_ensure is called from every
     * Music menu entry, and re-running the retries and sleeps on each
     * keypress against a failing disk would make the whole UI crawl. The
     * error is in g_lib_load_err; a deliberate retry clears g_lib_scanned. */
    g_lib_scanned = 1;
    build_artists();                       /* so About/Artists count is live */
    /* The On-The-Go list is locator pairs; what they resolve to is a function
     * of the library that has just been (re)built, so it is re-bound here and
     * nowhere else. Cheap: one hash lookup per entry. */
    otg_bind_all();
    g_lib_load_ms = (mmio_read32(USEC_TIMER_ADDR) - t0) / 1000u;
    /* Into the log, for the host's boot check: what the library came to and
     * how long it took. Hex like every other core: line (uart has no decimal). */
    uart_puts("core: library songs ");
    uart_put_hex32((uint32_t)g_songs_n);
    uart_puts(" albums ");
    uart_put_hex32((uint32_t)g_albums_n);
    uart_puts(" ms ");
    uart_put_hex32(g_lib_load_ms);
    uart_putc('\n');
}

/* Populate g_songview with the songs to show (genre < 0 = all), title-ordered. */
/*
 * Build the Songs view. `genre` < 0 means every genre; `artist` NULL means every
 * artist. Artist matching folds through artist_key()+title_cmp, the same
 * comparison build_artists() uses to merge "The Kid LAROI" with "Kid Laroi" —
 * so an artist's song list contains exactly the songs its Artists row counted.
 */
/* Whose songs the current view holds ("" = everyone's), for the header. */
char g_songview_artist[NAME_MAX + 1];
int  g_songview_kind;             /* RESUME_KIND_* of the current view */

void songview_build(int genre, const char *artist)
{
    g_list_epoch++;
    g_songview_kind = resume_kind_of_view(genre, artist);
    copy_display_name(g_songview_artist, artist ? artist : "", 0 /*keep ext*/);
    g_songview_n = 0;
    for (int i = 0; i < g_songs_n; i++) {
        int si = g_song_sorted[i];
        if (genre >= 0 && g_songs[si].genre != genre) {
            continue;
        }
        if (artist && title_cmp(artist_key(g_songs[si].artist),
                                artist_key(artist)) != 0) {
            continue;
        }
        g_songview[g_songview_n++] = (uint16_t)si;
    }
    g_song_sel = g_song_accum = 0;
}
