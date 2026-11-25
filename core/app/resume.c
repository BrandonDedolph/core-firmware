/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/resume.c — capture and restore of the playback position (and the
 * queue it was in) across a power cycle. Moved verbatim out of kernel/main.c.
 *
 * resume_find_song() stays `static` with its text unchanged:
 * tests/scripts/check_resume_parity.py compares it byte for byte with the
 * copy in tests/kernel/resume_test.c, and reads RESUME_DUR_SLOP from this
 * file, so that #define stays here and not in resume.h.
 */

#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/resume_ctx.h"
#include "../ui/settings.h"
#include "../library/names.h"
#include "../library/playlist.h"
#include "../library/otg.h"
#include "state.h"
#include "util.h"
#include "persist.h"
#include "library.h"
#include "playctx.h"
#include "playlists.h"
#include "albums.h"
#include "otg.h"
#include "lists.h"
#include "resume.h"

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

/* Below this, don't bother restoring a position — the user is at the top of
 * the track for any purpose they'd notice, and it saves a seek. */
#define RESUME_MIN_SECS    10u

/* How far the library's indexed duration may differ from the decoder's before
 * we stop believing the two describe the same file. Both are truncated to
 * whole seconds from different sources, so they disagree by a second on
 * perfectly good matches. */
#define RESUME_DUR_SLOP    2u

uint32_t g_resume_open_seq;      /* player_open_seq() when last captured */
uint32_t g_resume_last_us;       /* when we last considered capturing     */
uint32_t g_resume_end_seq;       /* last seen player_end_seq()            */
int      g_resume_was_paused;    /* pause state when last captured       */

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
void resume_restore(fat32_t *fs)
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
