/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/library.h — the in-RAM library: the album, artist and song tables
 * loaded at boot from CORELIB.IDX (or, without one, by a tag scan of the
 * disk), the lookups over them (album by folder cluster, song by file hash,
 * folder by name hash) and the on-screen album and song views. Moved
 * verbatim out of kernel/main.c.
 *
 * The tables are plain extern data for now (phase 2 of the split): the
 * screens, the queue builders, playlists, On-The-Go and resume all read
 * them directly, as they did inside main.c.
 */

#ifndef CORE_APP_LIBRARY_H
#define CORE_APP_LIBRARY_H

#include <stdint.h>

#include "../fs/fat32.h"
#include "../player/player.h"   /* NAME_MAX */

/* Index-derived album list — the source of truth is CORELIB.IDX, so an album
 * appears here only if the index references it. Stale/orphan or differently
 * named folders left on disk by an old import simply never show (which is what
 * kept "Kid Laroi" and "The Kid LAROI" from splitting into two entries). Built
 * once by library_ensure, sorted A->Z; g_albumview is the on-screen slice
 * (all, or one artist's). */
#define LIB_MAX_ALBUMS 1024
typedef struct {
    char     folder[NAME_MAX + 1];
    uint32_t clus;                       /* album folder cluster                  */
    uint32_t art_clus, art_size;         /* folder.art (120x120) — now-playing    */
    uint32_t thm_clus, thm_size;         /* folder.thm (28x28)  — list chip       */
    uint8_t  unreadable;                 /* the folder's directory could not be
                                          * read at load, even after retries. Its
                                          * songs have file_clus == 0 — NOT because
                                          * they left the disk, but because we never
                                          * got to look. Cleared (and the songs
                                          * bound) when a later open reads it. */
} lib_album_t;

#define ARTISTS_MAX 512
/* Each artist carries a representative album cover (its first album's) so the
 * Artists list can show a chip, same as albums — indexed, no per-row scan. */
typedef struct {
    char     name[NAME_MAX + 1];
    uint32_t thm_clus, thm_size, art_clus, art_size;
} lib_artist_t;

#define LIB_MAX_SONGS  6000
#define LIB_MAX_GENRES 128
#define LIB_TITLE_MAX  48
#define LIB_GENRE_MAX  24

#define LIB_ARTIST_MAX 40
/* Same cap as a browse row (NAME_MAX + NUL): once a song binds to its file,
 * file[] is the on-disk stem exactly as copy_display_name() produces it for
 * the row and the queue, so the two are byte-identical and hash alike. */
#define LIB_FILE_MAX   (NAME_MAX + 1)

typedef struct {
    char     title[LIB_TITLE_MAX];
    char     artist[LIB_ARTIST_MAX];
    /*
     * The track's filename, extension trimmed — for DISPLAY (the queue entry,
     * hence Now Playing) and nothing else. Until the record binds to a file it
     * holds the index's copy; after, the on-disk stem (resolve_art_cb).
     *
     * Never a key. The index field is the first 63 bytes of a name that may be
     * longer, and the device used to trim its extension by searching for the
     * LAST '.' in that truncated string: past ~68 bytes the ".flac" was gone,
     * the cut landed on an interior dot, and "16. TRAGIC (feat. ..." became
     * "16" — a name no directory entry could ever equal. The record still
     * played (file_hash bound it) but its row showed no duration, no title,
     * the wrong gutter number, and resume never found it. Every binding now
     * goes through a hash of the FULL on-disk name, or the cluster it bound.
     */
    char     file[LIB_FILE_MAX];
    uint32_t file_hash;                   /* name_hash of the FULL on-disk name,  */
                                          /* extension included: the record<->  */
                                          /* file locator (host-stamped; the    */
                                          /* scan path computes it at readdir)  */
    uint32_t stem_hash;                   /* name_hash of the on-disk stem — the  */
                                          /* resume locator. Provisional (from  */
                                          /* file[]) until the record binds     */
    uint32_t dir_clus;                    /* album folder (queue context + play)*/
    uint32_t file_clus, file_size;        /* the track file itself (resolved at   */
                                          /* load) — play/shuffle without a scan  */
    uint32_t duration_s;
    uint16_t track, disc;
    int16_t  genre;                       /* index into g_genres, -1 = none     */
    uint8_t  fmt;                         /* classify_ext of the ON-DISK name:  */
                                          /*   0 = FLAC, 1 = MP3. The index     */
                                          /*   record does not carry it (the    */
                                          /*   stored name is ext-trimmed), so  */
                                          /*   it is set where the dirent is in  */
                                          /*   hand: the resolve pass, or the   */
                                          /*   no-index scan                     */
} lib_song_t;

/* MUST be >= LIB_MAX_ALBUMS: a record whose album folder didn't make the map
 * resolves to cluster 0 and every one of its tracks is silently dropped at load
 * (the old 160 cap quietly lost albums 161..256 of a full library). */
#define FOLDER_MAP_MAX LIB_MAX_ALBUMS
/* g_folder_map's element. Named so the map can be declared below: two
 * anonymous struct declarations in one file are two different types. */
typedef struct { char name[NAME_MAX + 1]; uint32_t clus, hash; } lib_folder_t;

#define FOLDER_HASH_BUCKETS 2048         /* power of two > FOLDER_MAP_MAX      */

#define SONG_HASH_BUCKETS 2048           /* power of two > LIB_MAX_SONGS       */
#define ALBUM_HASH_BUCKETS 512           /* power of two > LIB_MAX_ALBUMS      */

#define LIB_READDIR_RETRIES   2      /* re-walks per directory, after the first  */

extern lib_album_t g_albums[LIB_MAX_ALBUMS];
extern int         g_albums_n;
extern uint16_t    g_albumview[LIB_MAX_ALBUMS];
extern int         g_albumview_n;
extern int         g_lib_truncated;
extern uint32_t     g_lib_load_ms;
extern lib_artist_t g_artists[ARTISTS_MAX];
extern int          g_artists_n;
extern lib_song_t g_songs[LIB_MAX_SONGS];
extern int        g_songs_n;
extern uint16_t   g_song_sorted[LIB_MAX_SONGS];
extern char       g_genres[LIB_MAX_GENRES][LIB_GENRE_MAX];
extern int        g_genres_n;
extern int        g_genre_count[LIB_MAX_GENRES];
extern lib_folder_t g_folder_map[FOLDER_MAP_MAX];
extern int      g_folder_n;
extern uint16_t g_folder_hh[FOLDER_HASH_BUCKETS];
extern uint16_t g_folder_hn[FOLDER_MAP_MAX];
extern uint16_t   g_songview[LIB_MAX_SONGS];
extern int        g_songview_n;
extern uint16_t g_song_hh[SONG_HASH_BUCKETS];
extern uint16_t g_song_hn[LIB_MAX_SONGS];
extern int g_lib_retry_budget;
extern uint32_t g_lib_root_clus;
extern uint16_t g_sort_tmp[LIB_MAX_SONGS];
extern char g_album_key[LIB_MAX_ALBUMS][NAME_MAX + 1];
extern char g_songview_artist[NAME_MAX + 1];
extern int  g_songview_kind;

void albumview_build(const char *artist_filter);
int album_by_clus(uint32_t dc);
uint32_t folder_clus_h(uint32_t hash, const char *name);
int lib_readdir(fat32_t *fs, uint32_t clus, fat32_dir_cb cb, void *ud,
                       int *acc_n);
int album_resolve(fat32_t *fs, int i);
void library_ensure(fat32_t *fs);
void songview_build(int genre, const char *artist);

#endif /* CORE_APP_LIBRARY_H */
