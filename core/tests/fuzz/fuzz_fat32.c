/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_fat32.c — fs/fat32.c over a whole mutated disk image.
 *
 * FEEDS: the input IS the disk, 512-byte LBA 0 first, partition at LBA 0 —
 * so a saved crash is a raw image you can hand to mtools or fsck as-is. The
 * seeds are tests/scripts/make_fat32_image.py's volumes (24 KiB each: the
 * good one and every --variant), small enough to mutate whole. Reads past
 * the end of the input fail with an error, the way a truncated volume or a
 * dead sector does, so geometry the fuzzer inflates cannot turn the walk
 * into a scan of gigabytes of implied zeros.
 *
 * DRIVES every reader entry point the firmware calls on a user's disk:
 * fat32_mount, a bounded recursive fat32_readdir from the root, fat32_open_in
 * on every name the walk surfaced, fat32_resolve_path over the walk's own
 * paths and some hostile ones, fat32_read_file, a fat32_stream_read /
 * fat32_stream_skip mix, fat32_file_lba and fat32_file_lba_at.
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - a surfaced name not NUL-terminated inside its field, or not UTF-8;
 *   - a name the walk surfaced that fat32_open_in then cannot find (lookup
 *     and enumeration are documented to be one walk and must not drift);
 *   - a read or stream call returning more than it was asked for, a short
 *     read mid-file, or a stream that yields more than the file's size;
 *   - fat32_file_lba[_at] reporting success with LBA 0 or no sectors, or
 *     failure without zeroing both out-params (the fail-closed rule the
 *     config/evlog/otg writers rely on);
 *   - more than READ_BUDGET block reads in one input: every walk is bounded
 *     by the volume, and this image is at most a few hundred blocks.
 */

#include "fuzz.h"

#include "../../fs/fat32.h"

#define MAX_IMAGE    (256u * 1024u)    /* the seeds are 24 KiB             */
#define READ_BUDGET  200000u
#define MAX_DEPTH    4
#define MAX_PER_DIR  48
#define READ_CAP     (64u * 1024u)

typedef struct {
    const uint8_t *img;
    size_t         len;
    uint32_t       reads;
} disk_t;

static int disk_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    disk_t *d = (disk_t *)ud;
    d->reads++;
    FUZZ_CHECK(d->reads < READ_BUDGET);
    FUZZ_CHECK(count != 0);
    uint64_t off = (uint64_t)lba * 512u, n = (uint64_t)count * 512u;
    if (off > d->len || n > d->len - off) {
        return -1;                       /* past the end: a dead sector */
    }
    memcpy(buf, d->img + off, (size_t)n);
    return 0;
}

static uint8_t g_buf[READ_CAP];
static const uint8_t *g_ctl;             /* steering bytes for the stream mix */
static size_t g_ctl_len, g_ctl_pos;

static uint32_t ctl_next(void)
{
    if (g_ctl_len == 0) return 0;
    uint32_t v = g_ctl[g_ctl_pos % g_ctl_len];
    g_ctl_pos += 7u;
    return v;
}

static void check_lba(fat32_t *fs, uint32_t clus, uint32_t off)
{
    uint32_t lba = 0xDEADu, max = 0xDEADu;
    int rc = fat32_file_lba(fs, clus, &lba, &max);
    if (rc == 0) {
        FUZZ_CHECK(lba != 0 && max != 0);
        FUZZ_CHECK(max == fs->sec_per_clus * fs->sec_ratio);
    } else {
        FUZZ_CHECK(rc < 0 && lba == 0 && max == 0);
    }
    lba = 0xDEADu; max = 0xDEADu;
    rc = fat32_file_lba_at(fs, clus, off, &lba, &max);
    if (rc == 0) {
        FUZZ_CHECK(off % 512u == 0);
        FUZZ_CHECK(lba != 0 && max != 0);
        FUZZ_CHECK(max <= fs->sec_per_clus * fs->sec_ratio);
    } else {
        FUZZ_CHECK(rc < 0 && lba == 0 && max == 0);
    }
}

static void check_file(fat32_t *fs, const fat32_dirent_t *e)
{
    uint32_t want = e->size < READ_CAP ? e->size : READ_CAP;
    int32_t got = fat32_read_file(fs, e->first_clus, g_buf, want);
    FUZZ_CHECK(got <= (int32_t)want);

    /* Stream it in uneven pieces with skips between, as the decoders do. */
    fat32_stream_t st;
    fat32_stream_open(&st, fs, e->first_clus, e->size);
    uint64_t total = 0;
    for (int step = 0; step < 64; step++) {
        uint32_t c = ctl_next();
        if (c & 1u) {
            uint32_t n = (c << 6) + 1u;
            uint32_t before = st.remaining;
            uint32_t sk = fat32_stream_skip(&st, n);
            FUZZ_CHECK(sk <= n && sk <= before);
            total += sk;
        } else {
            uint32_t n = ((c >> 1) * 97u) % READ_CAP + 1u;
            uint32_t before = st.remaining;
            int32_t r = fat32_stream_read(&st, g_buf, n);
            if (r < 0) break;
            FUZZ_CHECK((uint32_t)r <= n);
            /* short only at end of file */
            FUZZ_CHECK((uint32_t)r == n || (uint32_t)r == before);
            total += (uint32_t)r;
            if (r == 0) break;
        }
    }
    FUZZ_CHECK(total <= e->size);
    check_lba(fs, e->first_clus, (ctl_next() & 0x3Fu) * 512u);
}

typedef struct {
    fat32_dirent_t ent[MAX_PER_DIR];
    int            n;
} listing_t;

static int collect_cb(void *ud, const fat32_dirent_t *e)
{
    listing_t *l = (listing_t *)ud;
    FUZZ_CHECK(fuzz_terminated(e->name, sizeof e->name));
    FUZZ_CHECK(fuzz_terminated(e->short_name, sizeof e->short_name));
    /*
     * KNOWN GAP (fs/fat32.c, fmt_83): an 8.3 name is copied byte for byte,
     * and a byte >= 0x80 there is in the volume's OEM code page (CP437/850),
     * not UTF-8 — so `name` and `short_name` are not the UTF-8 fat32.h
     * promises. Cosmetic on the device (ui/text.c renders a bad sequence as
     * U+FFFD) and rare (Windows writes an LFN for any non-ASCII name), but it
     * is the one way a name reaches the hash/renderer un-normalised. The fix
     * is a code-page decision (CP437? CP850? refuse?) that also has to match
     * tools/build_index.py, so it is not guessed at here.
     * Regression input: regress/fat32/xfail-oem-short-name.
     */
    FUZZ_XFAIL(!fuzz_utf8_ok(e->short_name, 1),
               "fs/fat32.c fmt_83: 8.3 names with bytes >= 0x80 are OEM code "
               "page, not UTF-8");
    FUZZ_CHECK(fuzz_utf8_ok(e->name, 1));
    FUZZ_CHECK(e->is_dir == 0 || e->is_dir == 1);
    if (l->n < MAX_PER_DIR) {
        l->ent[l->n++] = *e;
    }
    return l->n >= MAX_PER_DIR;          /* stop early: also a contract */
}

static char g_path[FAT32_PATH_MAX];

static void walk(fat32_t *fs, uint32_t dir_clus, int depth, size_t plen)
{
    static listing_t lists[MAX_DEPTH];
    listing_t *l = &lists[depth];
    l->n = 0;
    (void)fat32_readdir(fs, dir_clus, collect_cb, l);

    for (int i = 0; i < l->n; i++) {
        const fat32_dirent_t *e = &l->ent[i];
        uint32_t clus = 0, size = 0;
        if (!e->name_lossy) {
            /* If the walk saw it, the lookup must find it. A directory read
             * that fails AFTER the entry is the one excuse: open_in reports
             * that read error rather than ENOENT. */
            int rc = fat32_open_in(fs, dir_clus, e->name, &clus, &size);
            FUZZ_CHECK(rc != FAT32_ENOENT);
        }

        /* Build "<parent>/<name>" and resolve it from the root. */
        size_t nl = strlen(e->name);
        if (plen + 1u + nl + 1u < sizeof g_path) {
            size_t p = plen;
            if (p) g_path[p++] = '/';
            memcpy(g_path + p, e->name, nl + 1u);
            fat32_dirent_t out;
            uint32_t parent = 0;
            (void)fat32_resolve_path(fs, fs->root_clus, g_path, &out, &parent);
            if (e->is_dir && depth + 1 < MAX_DEPTH && e->first_clus != dir_clus) {
                walk(fs, e->first_clus, depth + 1, p + nl);
            }
            g_path[plen] = '\0';
        }
        if (!e->is_dir) {
            check_file(fs, e);
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 512u || size > MAX_IMAGE) return 0;

    disk_t d = { data, size, 0 };
    fat32_t fs;
    fat32_cache_drop(NULL);              /* caches are keyed by &fs: drop them */
    memset(&fs, 0, sizeof fs);
    if (fat32_mount(&fs, disk_read, &d, 0) != 0) return 0;

    /* The volume's own bytes steer the stream mix: the last 64 bytes of the
     * image, which are file data in every seed. */
    g_ctl = data + size - 64u;
    g_ctl_len = 64u;
    g_ctl_pos = 0;

    g_path[0] = '\0';
    walk(&fs, fs.root_clus, 0, 0);

    /* Hostile paths: the walk never reaches above its start, never accepts
     * dot segments, and is bounded on an unterminated or deep path. */
    static const char *const odd[] = {
        "", "/", "..", "a/../b", "./x", "//HELLO.TXT//", "\\HELLO.TXT",
        "a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q", "INTENT~1.FLA", "intentions.FLAC",
    };
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        fat32_dirent_t out;
        uint32_t parent = 0;
        int rc = fat32_resolve_path(&fs, fs.root_clus, odd[i], &out, &parent);
        /* the first five have no segments or a dot segment: never found */
        if (i < 5) FUZZ_CHECK(rc != 0);
    }

    /* The address resolver against arbitrary cluster numbers and offsets. */
    for (uint32_t k = 0; k < 8u; k++) {
        uint32_t clus = ctl_next() | (ctl_next() << 8);
        check_lba(&fs, clus, (ctl_next() & 0x7Fu) * 512u);
    }
    return 0;
}
