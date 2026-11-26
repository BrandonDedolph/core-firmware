/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/actions.c — what PLAY and SELECT do on a list row. Moved verbatim
 * out of kernel/main.c; see actions.h.
 */

#include <stdint.h>
#include "hw/volume.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/resume_ctx.h"
#include "../ui/gesture.h"
#include "../ui/keyhold.h"
#include "../ui/search.h"
#include "../library/names.h"
#include "../library/otg.h"
#include "state.h"
#include "util.h"
#include "nav.h"
#include "library.h"
#include "playctx.h"
#include "playlists.h"
#include "albums.h"
#include "nowplaying.h"
#include "otg.h"
#include "lists.h"
#include "search_glue.h"
#include "actions.h"

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
play_tap_t play_tap_start(fat32_t *fs)
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
rowsel_t g_rowsel;

/*
 * Two screens are really two lists behind one screen_t, and a press has to
 * belong to the one it started on: SCR_BROWSER is the album list at depth 0
 * and one album's tracks at depth 1, and SCR_SEARCH is the character ring
 * (where SELECT types and never arbitrates) or the hits.
 */
int rowsel_sub(void)
{
    switch (scr_cur()) {
    case SCR_BROWSER: return g_dir_depth;
    case SCR_SEARCH:  return g_search.mode;
    default:          return 0;
    }
}

void rowsel_arm(int sel, keyhold_t *k, uint32_t now_us)
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

void rowsel_drop(keyhold_t *k)
{
    g_rowsel.pending = 0;
    keyhold_reset(k);
}

int row_select_tap(fat32_t *fs)
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
int row_select_hold(fat32_t *fs)
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
