/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/index_loader_test.c — the REAL app/library.c
 * library_load_index() against damaged copies of a CORELIB.IDX.
 *
 * library/idx.c's validators (idx_header_parse, crc32_update) are unit-tested
 * by tests/library/idx_test.c. What that test cannot show is that the loader
 * USES them, with the right inputs, and does the right thing with a refusal:
 * a validator the loader does not consult is decoration. So this boots the
 * headless firmware on the A-Z disk (its index holds more than one 16 KB
 * batch of records and spans several clusters), each case on its own copy
 * of the image with one thing wrong, and reads the outcome off the UART
 * ("idx: rejected (<why>)", "core: library songs N"):
 *
 *   intact      the image as built: loaded from the index, every record.
 *   padded      the directory entry says 256 bytes MORE than the header's
 *               count accounts for: refused (size). Only the directory
 *               entry's size can show this — the header agrees with itself —
 *               so it fails if the loader hands idx_header_parse anything but
 *               g_idx_size. The tag scan then builds the library.
 *   truncated   256 bytes LESS: refused (size), before any record is read.
 *   crc         one byte of the last record flipped: refused (crc), the
 *               scan takes over, and the flipped title is nowhere.
 *   chain       the file's cluster chain ends after its first cluster (the
 *               directory entry still promises the rest): a record read that
 *               fails is refused (read) and the scan takes over — never a
 *               library of the records that happened to read, which is what
 *               `if (got <= 0) break;` used to produce.
 *   eio-once    one read of the index's second cluster fails at the drive
 *               (every one of player_disk_read's attempts): retried on the
 *               load's budget, and the index loads in full.
 *   eio-always  it never reads: refused (read) with the load error set — the
 *               DISK failed, so no scan fallback against it: zero songs.
 *
 * This replaces tests/scripts/check_index_loader.py, which asserted the same
 * by reading library.c as text.
 *
 * Usage: index_loader_test DISK_AZ.img
 */

#include "headless.h"
#include "../xfail.h"
#include "app/library.h"        /* g_songs: what the load produced */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static xfail_ctx T = { "index-loader", 0, 0, 0 };
#define CHECK(label, cond) xpect(&T, (label), (cond) ? 1 : 0)

/* ---- the image: where CORELIB.IDX is ------------------------------------- */

static uint8_t *g_img;
static size_t   g_img_n;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | (rd16(p + 2) << 16);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint64_t part;          /* byte offset of the FAT32 volume             */
    uint32_t bps, spc;      /* FS sector bytes, sectors per cluster        */
    uint64_t fat0, fat1;    /* byte offsets of the two FATs                */
    uint64_t data;          /* byte offset of cluster 2                    */
    uint64_t dirent;        /* byte offset of CORELIB.IDX's 8.3 entry      */
    uint32_t clus, size;    /* its first cluster and size                  */
    uint32_t count;         /* records, from the index header              */
    uint32_t hdr_len;
} idx_loc_t;

static idx_loc_t g_loc;

static uint64_t clus_off(uint32_t c)
{
    return g_loc.data + (uint64_t)(c - 2) * g_loc.spc * g_loc.bps;
}

static int locate(void)
{
    const uint8_t *mbr = g_img;
    g_loc.part = (uint64_t)rd32(mbr + 0x1BE + 8) * 512u;
    const uint8_t *bs = g_img + g_loc.part;
    g_loc.bps = rd16(bs + 11);
    g_loc.spc = bs[13];
    uint32_t rsvd = rd16(bs + 14), nfats = bs[16], fatsz = rd32(bs + 36);
    if (nfats != 2 || g_loc.bps == 0 || g_loc.spc == 0) return -1;
    g_loc.fat0 = g_loc.part + (uint64_t)rsvd * g_loc.bps;
    g_loc.fat1 = g_loc.fat0 + (uint64_t)fatsz * g_loc.bps;
    g_loc.data = g_loc.fat0 + (uint64_t)nfats * fatsz * g_loc.bps;
    /* The 8.3 entry: the only "CORELIB IDX" short name on the volume. */
    g_loc.dirent = 0;
    for (uint64_t o = g_loc.data; o + 32 <= g_img_n; o += 32) {
        if (memcmp(g_img + o, "CORELIB IDX", 11) == 0 && (g_img[o + 11] & 0x10) == 0) {
            g_loc.dirent = o;
            break;
        }
    }
    if (g_loc.dirent == 0) return -1;
    const uint8_t *e = g_img + g_loc.dirent;
    g_loc.clus = (rd16(e + 20) << 16) | rd16(e + 26);
    g_loc.size = rd32(e + 28);
    const uint8_t *h = g_img + clus_off(g_loc.clus);
    if (memcmp(h, "CIDX", 4) != 0) return -1;
    g_loc.hdr_len = rd16(h + 4) == 2 ? 16u : 12u;
    g_loc.count   = rd32(h + 8);
    return 0;
}

/* ---- one boot per case --------------------------------------------------- */

static int log_has(const char *s) { return strstr(hl_log(), s) != NULL; }

/* The "core: library songs N" line library_ensure prints (hex), or -1. A
 * scan finds every indexed song (and on this disk one file the index does
 * not list), so "the scan ran" is songs >= the index's record count. */
static long lib_songs(void)
{
    const char *p = strstr(hl_log(), "core: library songs ");
    if (!p) return -1;
    return strtol(p + strlen("core: library songs "), NULL, 16);
}

static int write_disk(const char *path, const uint8_t *img)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < g_img_n) {
        ssize_t n = write(fd, img + off, g_img_n - off);
        if (n <= 0) { close(fd); return -1; }
        off += (size_t)n;
    }
    return close(fd);
}

typedef enum {
    C_INTACT, C_PADDED, C_TRUNCATED, C_CRC, C_CHAIN, C_EIO_ONCE, C_EIO_ALWAYS,
} case_t;

static const char *const CASE_NAME[] = {
    "intact", "padded", "truncated", "crc", "chain", "eio-once", "eio-always",
};

/* A title in the last record, flipped to this by the crc case — so if the
 * damaged index were loaded anyway, this is the string it would show. */
static char g_flipped[49];

static _Noreturn void run_case(case_t c)
{
    char suite[64];
    snprintf(suite, sizeof suite, "index-loader %s", CASE_NAME[c]);
    T.suite = suite;

    uint8_t *img = malloc(g_img_n);
    if (!img) _exit(2);
    memcpy(img, g_img, g_img_n);
    uint8_t *e = img + g_loc.dirent;
    uint32_t clus_bytes = g_loc.spc * g_loc.bps;

    switch (c) {
    case C_INTACT:
        break;
    case C_PADDED:
        wr32(e + 28, g_loc.size + 256u);
        break;
    case C_TRUNCATED:
        wr32(e + 28, g_loc.size - 256u);
        break;
    case C_CRC: {
        /* The last record's title (record offset 136, 48 bytes): its first
         * byte changed, so the record still parses — only the CRC knows. */
        uint8_t *rec = img + clus_off(g_loc.clus) + g_loc.hdr_len +
                       (uint64_t)(g_loc.count - 1) * 256u;
        rec[136] ^= 0x20;
        memcpy(g_flipped, rec + 136, 48);
        g_flipped[48] = '\0';
        break;
    }
    case C_CHAIN:
        wr32(img + g_loc.fat0 + (uint64_t)g_loc.clus * 4u, 0x0FFFFFFFu);
        wr32(img + g_loc.fat1 + (uint64_t)g_loc.clus * 4u, 0x0FFFFFFFu);
        break;
    case C_EIO_ONCE:
    case C_EIO_ALWAYS:
        break;
    }

    char disk[] = "/tmp/index_loader_XXXXXX";
    int fd = mkstemp(disk);
    if (fd < 0 || write_disk(disk, img) != 0) {
        fprintf(stderr, "cannot write %s\n", disk);
        _exit(2);
    }
    close(fd);
    free(img);

    /* The index's second cluster (files are allocated contiguously, which
     * the chain case's EOC on the first cluster relies on too). */
    uint32_t second_lba = (uint32_t)(clus_off(g_loc.clus + 1) / 512u);
    uint32_t clus_lbas  = clus_bytes / 512u;
    if (c == C_EIO_ONCE) {
        hl_fail_reads(second_lba, clus_lbas, 6);     /* player_disk_read's 6 */
    } else if (c == C_EIO_ALWAYS) {
        hl_fail_reads(second_lba, clus_lbas, 0xFFFFFFFFu);
    }

    if (hl_boot(disk, 12000) != 0) {
        fprintf(stderr, "hl_boot failed\n");
        unlink(disk);
        _exit(2);
    }
    hl_tap(HL_BTN_SELECT, 3000);       /* main menu -> Music: library_ensure */
    unlink(disk);

    long songs = lib_songs();
    CHECK("the library load finished", songs >= 0);

    switch (c) {
    case C_INTACT:
        CHECK("fixture: the index is more than one 16 KB batch of records",
              g_loc.count > 64);
        CHECK("fixture: ...and more than one cluster",
              g_loc.size > clus_bytes);
        CHECK("the index is accepted", !log_has("idx: rejected"));
        CHECK("every record is a song", songs == (long)g_loc.count);
        break;
    case C_PADDED:
    case C_TRUNCATED:
        CHECK("refused on the directory entry's size",
              log_has("idx: rejected (size)"));
        CHECK("the tag scan built the library instead",
              songs >= (long)g_loc.count);
        break;
    case C_CRC:
        CHECK("refused on the record CRC", log_has("idx: rejected (crc)"));
        CHECK("the tag scan built the library instead",
              songs >= (long)g_loc.count);
        break;
    case C_CHAIN:
        CHECK("a failed record read refuses the index",
              log_has("idx: rejected (read)"));
        CHECK("the tag scan built the whole library instead",
              songs >= (long)g_loc.count);
        break;
    case C_EIO_ONCE:
        CHECK("a transient read error is retried, not refused",
              !log_has("idx: rejected"));
        CHECK("every record is a song", songs == (long)g_loc.count);
        break;
    case C_EIO_ALWAYS:
        CHECK("a read error that persists refuses the index",
              log_has("idx: rejected (read)"));
        CHECK("...as a DISK error: no scan fallback, no songs", songs == 0);
        break;
    }
    if (c == C_CRC) {
        int seen = 0;
        for (int i = 0; i < g_songs_n; i++) {
            if (strcmp(g_songs[i].title, g_flipped) == 0) seen = 1;
        }
        CHECK("...and the damaged record's title is nowhere in it", !seen);
    }
    if (T.fails) {
        fprintf(stderr, "---- UART (%s) ----\n%s\n", CASE_NAME[c], hl_log());
    }
    fflush(NULL);
    _exit(T.fails ? 1 : 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s DISK_AZ.img\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    g_img_n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_img = malloc(g_img_n);
    if (!g_img || fread(g_img, 1, g_img_n, f) != g_img_n) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    if (locate() != 0) {
        fprintf(stderr, "index-loader: no CORELIB.IDX on %s\n", argv[1]);
        return 2;
    }
    printf("index-loader: CORELIB.IDX cluster %u, %u bytes, %u records\n",
           g_loc.clus, g_loc.size, g_loc.count);

    int failed = 0;
    for (case_t c = C_INTACT; c <= C_EIO_ALWAYS; c++) {
        fflush(NULL);
        pid_t p = fork();
        if (p == 0) run_case(c);
        int st = 0;
        waitpid(p, &st, 0);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            fprintf(stderr, "index-loader: case %s FAILED\n", CASE_NAME[c]);
            failed++;
        }
    }
    return failed ? 1 : 0;
}
