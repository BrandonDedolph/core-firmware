/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/otg.c — On-The-Go: the live list's binding to the library, the
 * add/remove/play actions, the confirmation banner's state, the SCR_OTG
 * screen and Save/Delete against the five slot playlists. Moved verbatim out
 * of kernel/main.c.
 */

#include <stdint.h>
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/ata.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "../kernel/console.h"
#include "../kernel/resume_ctx.h"
#include "../kernel/cfg_commit.h"
#include "../kernel/otg_store.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "../library/playlist.h"
#include "../library/otg.h"
#include "../library/otg_slot.h"
#include "state.h"
#include "layout.h"
#include "util.h"
#include "bootscreen.h"
#include "persist.h"
#include "nav.h"
#include "library.h"
#include "playctx.h"
#include "statusbar.h"
#include "playlists.h"
#include "main_internal.h"
#include "otg.h"

/* ---------------------------------------------------------------------------
 * On-The-Go, the parts the Playlists screens need. The rest of the feature is
 * a section of its own below (it needs the screen stack); these few things sit
 * here because Playlists pins an On-The-Go row and a saved slot's tracklist
 * carries a Delete row, and both are painted from here.
 * ------------------------------------------------------------------------- */
otg_list_t g_otg;                  /* the live list, 4100 B of .bss   */
int16_t    g_otg_song[OTG_MAX];    /* the g_songs index each entry
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

ui_window_t g_otg_confirm;       /* Clear Playlist, on SCR_OTG        */

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

int                g_otg_sel, g_otg_accum; /* the On-The-Go screen      */
static otg_save_row_t     g_otg_rows[OTG_MAX];    /* Save's row array, 4 KB    */
static otg_entry_t        g_otg_batch[OTG_MAX];   /* one album, in order, 4 KB */
static int16_t            g_otg_order[OTG_MAX];   /* ...sorted first, 1 KB     */
static otg_save_scratch_t g_otg_save_scr;         /* ~9.7 KB of .bss, one copy */
static otg_save_stats_t   g_otg_save_stats;
static int                g_otg_slot_lba_said;    /* the first-Save UART line  */

/* The pending change to COREOTG.DAT. The gate itself is inside
 * kernel/otg_store.c (as the event log's is inside evlog.c), so this file only
 * gathers what the gate needs and narrates the verdict. */
void otg_touch(void)
{
    otg_store_touch(mmio_read32(USEC_TIMER_ADDR));
}

int otg_row_count(void) { return OTG_ROW_FIRST + (int)g_otg.n; }

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

ui_window_t g_otg_flash;
int8_t      g_otg_flash_sign;            /* +1 plus, -1 minus, 0 none  */
char        g_otg_flash_label[40];
char        g_otg_flash_token[16];

/* "<n> SONGS" / "1 SONG" — the small-caps token where a list header's count
 * would be. */
void otg_count_token(char *dst, int n)
{
    int i = u32_to_dec(dst, (unsigned)n);
    otg_str_copy(dst + i, 16u - (uint32_t)i, n == 1 ? " SONG" : " SONGS");
}

void otg_flash(int sign, const char *label, const char *token)
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
int otg_add_song(int si)
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

int otg_add_album(int ai)
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
int otg_play(int start)
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
int otg_slot_free_index(fat32_t *fs)
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
int g_otg_free_slot;

void otg_row_draw(int r, int idx)
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
void otg_header_right(char *right, int sel)
{
    right[0] = '\0';
    if (sel >= OTG_ROW_FIRST && g_otg.n > 0) {
        fmt_count(right, sel - OTG_ROW_FIRST + 1, (int)g_otg.n);
    } else if (g_otg_missing > 0) {
        int k = u32_to_dec(right, (unsigned)g_otg_missing);
        otg_str_copy(right + k, 12u - (uint32_t)k, " MISSING");
    }
}

void otg_render(int sel)
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
int otg_save_to_slot(fat32_t *fs)
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
int otg_delete_slot(fat32_t *fs)
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
