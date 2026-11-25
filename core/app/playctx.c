/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/playctx.c — see playctx.h.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/resume_ctx.h"
#include "../ui/settings.h"
#include "state.h"
#include "bootscreen.h"
#include "library.h"
#include "playctx.h"

/*
 * What the player's queue was built FROM, for the resume capture: the kind
 * (RESUME_KIND_*) and, for Shuffle Songs, the library-order seed. Every
 * builder below sets both; player_jump keeps them (same queue). Together
 * with the song's own artist/genre fields and the player's order seed they
 * are enough to build the same queue again at boot (resume_restore).
 */
int      g_queue_kind = RESUME_KIND_NONE;
uint32_t g_queue_seed;
/* For RESUME_KIND_PLAYLIST only: name_hash of the playlist's ext-trimmed
 * filename — the one word that names WHICH playlist, since nothing in the
 * song's own record does. Meaningless (and captured as 0) for other kinds. */
uint32_t g_queue_ctx_hash;

/*
 * Fill one queue entry from a library record. The Songs-view builder and the
 * Shuffle Songs builder were two copies of the same nine lines, and
 * browse_entry_t is never memset — a field one copy forgot would carry over
 * from whatever listing used the struct last. One authority. (playlist_play
 * fills its own: a playlist row is a playlist_track_t, not a lib_song_t.)
 *
 * The album grouping is the index's: album_by_clus() is already resolved here
 * for the cover art, and +1 leaves 0 free to mean "the album table does not
 * know this folder" — a stray, which Shuffle Albums plays on its own rather
 * than folding in with other strays.
 */
void queue_entry_from_song(browse_entry_t *e, const lib_song_t *s)
{
    int k = 0;
    for (; s->file[k] && k < NAME_MAX; k++) e->name[k] = s->file[k];
    e->name[k]  = '\0';
    e->clus     = s->file_clus;
    e->size     = s->file_size;
    e->fmt      = s->fmt;                  /* FLAC or MP3, from the on-disk name */
    e->is_dir   = 0;
    int ai = album_by_clus(s->dir_clus);
    e->art_clus = (ai >= 0) ? g_albums[ai].art_clus : 0;
    e->art_size = (ai >= 0) ? g_albums[ai].art_size : 0;
    e->album    = (ai >= 0) ? (uint16_t)(ai + 1) : 0;
    e->order_key = ((uint32_t)s->disc << 16) | s->track;
}

/* Play a song picked on the Songs list: the queue is the ENTIRE current song
 * view (all songs, in the displayed order), started at the picked track — so
 * "N of M" is the song's position in the whole library and Prev/Next walk every
 * song, not just the one album. (Same full-queue build as Shuffle Songs, minus
 * the shuffle.) Returns the queue index the pick landed on, -1 if nothing was
 * queued.
 *
 * Does NOT un-mute: the caller re-applies g_volume over the codec re-init.
 * The boot-time restore runs this under a mute, and an un-mute in here would
 * land between the DAC starting and the pause — an audible burst of the
 * track on every power-up. */
int library_play_song(fat32_t *fs, int songview_idx)
{
    (void)fs;
    if (songview_idx < 0 || songview_idx >= g_songview_n) return -1;
    uint16_t sel_song = g_songview[songview_idx];

    load_bar_begin();
    g_queue_kind = g_songview_kind;
    g_queue_seed = 0;
    /* Shuffle is the user's setting, not something picking a song turns off:
     * forcing it off here left the player un-shuffled for the rest of the
     * session while the UI kept showing the SHUF token. */
    player_set_shuffle(g_settings.shuffle);
    player_queue_begin();
    int start = 0, added = 0;
    for (int i = 0; i < g_songview_n; i++) {
        if ((i & 255) == 0) load_bar_progress("LOADING SONGS", i * 100 / g_songview_n);
        lib_song_t *s = &g_songs[g_songview[i]];
        /* Record the start position BEFORE the resolved-check: a pick that the
         * index lists but the disk no longer has would otherwise leave start at
         * 0 and play the top of the list. Pointing at `added` lands on the next
         * resolved song instead. */
        if (g_songview[i] == sel_song) start = added;
        if (!s->file_clus) continue;              /* unresolved on disk — skip */
        browse_entry_t e;
        queue_entry_from_song(&e, s);
        player_queue_add(&e);
        added++;
    }
    if (start >= added) start = (added > 0) ? added - 1 : 0;   /* nothing after it */
    if (added == 0) return -1;
    player_queue_commit(start);
    return start;
}

/*
 * Build the Shuffle Songs queue: the WHOLE library (not a sample) in the
 * order lib_shuffle_order() deals from `seed`, and start it at song index
 * `start_si` (-1 = the first). Returns the queue index that song landed on,
 * -1 when the library is empty or the song is not on the disk.
 *
 * The queue IS the shuffle, so the player is told to walk it in queue order
 * (PLAYER_KEEP_QUEUE): the Queue view then shows what will actually play.
 * It used to force the player's shuffle off for the build and push the
 * user's setting back afterwards, which is an off->on edge — the player
 * dealt a second permutation over the already-shuffled queue, and the view
 * and the playback disagreed. The setting is pushed BEFORE the build now,
 * so the SHUF token stays honest and there is exactly one deal, which the
 * KEEP_QUEUE re-deal then replaces with the queue's own order.
 *
 * Mixed albums => no single cover, so the queue-level art is left empty and
 * each entry carries its own. Does NOT un-mute (see library_play_song).
 */
int shuffle_songs_build(fat32_t *fs, uint32_t seed, int start_si)
{
    library_ensure(fs);
    if (g_songs_n == 0) return -1;

    load_bar_begin();
    g_queue_kind = RESUME_KIND_SHUFFLE;
    g_queue_seed = seed;

    static uint16_t ord[LIB_MAX_SONGS];
    int ns = g_songs_n;
    lib_shuffle_order(ord, ns, seed);

    /* Build the full queue from the resolved song index (file cluster + per-track
     * album cover) — every song, in the shuffled order. The Queue view then shows
     * that order, and Now Playing shows each song's own art. */
    player_set_shuffle(g_settings.shuffle);
    player_queue_begin();
    int start = -1, added = 0;
    for (int i = 0; i < ns; i++) {
        if ((i & 255) == 0) load_bar_progress("SHUFFLING SONGS", i * 100 / ns);
        lib_song_t *s = &g_songs[ord[i]];
        if (!s->file_clus) continue;       /* unresolved (missing on disk) — skip */
        if (ord[i] == start_si) start = added;
        browse_entry_t e;
        queue_entry_from_song(&e, s);
        player_queue_add(&e);
        added++;
    }
    if (added == 0) return -1;
    player_queue_commit(start < 0 ? 0 : start);
    player_reshuffle_with_seed(0, PLAYER_KEEP_QUEUE);   /* the queue is the order */
    return start;
}

/* "Shuffle Songs" from the Music menu: a fresh random draw each time it's
 * chosen. The seed is the free-running timer, ORed with 1 so it is never 0
 * (0 is "no seed" in the settings record). */
void shuffle_songs_play(fat32_t *fs)
{
    (void)shuffle_songs_build(fs, mmio_read32(USEC_TIMER_ADDR) | 1u, -1);
}
