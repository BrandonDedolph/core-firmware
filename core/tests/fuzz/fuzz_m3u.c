/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_m3u.c — fs/m3u.c, the playlist reader.
 *
 * FEEDS: byte 0 steers the parse — bits 0-2 pick the source's chunk size
 * (1, 2, 3, 7, 64, 511, 512 or unlimited bytes per call: the line assembler
 * and the BOM detector must not care where a read boundary falls), bits 3-4
 * the base directory (root, "Music/Rock", a messy "\Music\\Rock\", and one
 * three levels deep so '..' has somewhere to go), bits 5-7 out_max (0 — the
 * counting pass — up to 7). Byte 1 non-zero makes the source fail after
 * byte1 * 8 bytes. The rest is the playlist text.
 *
 * Then the same bytes, unchunked and unfailed, are parsed a second time
 * through m3u_parse_file() off an in-RAM FAT32 volume (fuzz_vol.c), and the
 * two results must agree — the disk adapter is documented to be the same
 * parse, and a read granularity that changed the result would be a bug.
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - a return other than M3U_OK, or M3U_EIO when the source failed;
 *   - count > out_max, count > total, or `truncated` disagreeing with
 *     total > count; bytes beyond the input or M3U_FILE_MAX;
 *   - an entry whose path is empty, unterminated, longer than M3U_PATH_MAX,
 *     or not canonical: a leading '/', a '\', an empty, "." or ".." segment
 *     (the reader promises paths a resolver can walk without re-checking);
 *   - an unterminated title, or a duration below -1;
 *   - the chunked and the on-disk parse disagreeing.
 */

#include "fuzz.h"

#include "../../fs/m3u.h"
#include "fuzz_vol.h"

#define OUT_MAX 8u

typedef struct {
    const uint8_t *p;
    uint32_t       n, off, chunk, fail_at;
    uint32_t       calls;
} src_t;

static int32_t src_fn(void *ud, void *buf, uint32_t len)
{
    src_t *s = (src_t *)ud;
    s->calls++;
    /* m3u_parse is bounded by M3U_FILE_MAX bytes; at one byte per call that
     * is the most calls it can need, plus the final 0. */
    FUZZ_CHECK(s->calls <= M3U_FILE_MAX + 2u);
    FUZZ_CHECK(len != 0 && buf != NULL);
    if (s->fail_at && s->off >= s->fail_at) return -1;
    if (s->chunk && len > s->chunk) len = s->chunk;
    uint32_t n = s->n - s->off;
    if (n > len) n = len;
    if (n) memcpy(buf, s->p + s->off, n);
    s->off += n;
    return (int32_t)n;
}

static void check_entry(const m3u_entry_t *e)
{
    FUZZ_CHECK(fuzz_terminated(e->path, sizeof e->path));
    FUZZ_CHECK(fuzz_terminated(e->title, sizeof e->title));
    FUZZ_CHECK(e->duration_s >= -1);
    size_t n = strlen(e->path);
    FUZZ_CHECK(n > 0 && n <= M3U_PATH_MAX);
    FUZZ_CHECK(e->path[0] != '/');
    FUZZ_CHECK(strchr(e->path, '\\') == NULL);
    /* segment by segment: none empty, none "." or ".." */
    const char *seg = e->path;
    for (;;) {
        const char *end = strchr(seg, '/');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);
        FUZZ_CHECK(len != 0);
        FUZZ_CHECK(!(len == 1 && seg[0] == '.'));
        FUZZ_CHECK(!(len == 2 && seg[0] == '.' && seg[1] == '.'));
        if (!end) break;
        seg = end + 1;
    }
}

static void check_result(const m3u_result_t *r, uint32_t out_max, size_t in_len,
                         const m3u_entry_t *out)
{
    FUZZ_CHECK(r->count <= out_max);
    FUZZ_CHECK(r->count <= r->total);
    FUZZ_CHECK(r->truncated == (r->total > r->count ? 1 : 0));
    FUZZ_CHECK(r->bytes <= in_len && r->bytes <= M3U_FILE_MAX);
    for (uint32_t i = 0; i < r->count; i++) check_entry(&out[i]);
}

static m3u_entry_t   g_out[OUT_MAX], g_out2[OUT_MAX];
static m3u_scratch_t g_scr;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2u) return 0;
    static const uint32_t chunks[8] = { 1, 2, 3, 7, 64, 511, 512, 0 };
    static const char *const bases[4] = {
        "", "Music/Rock", "\\Music\\\\Rock\\", "Music/A/B",
    };
    uint8_t  ctl     = data[0];
    uint32_t fail_at = (uint32_t)data[1] * 8u;
    uint32_t out_max = (uint32_t)(ctl >> 5);
    const char *base = bases[(ctl >> 3) & 3u];
    const uint8_t *text = data + 2;
    uint32_t tlen = (uint32_t)(size - 2u);

    src_t s = { text, tlen, 0, chunks[ctl & 7u], fail_at, 0 };
    m3u_result_t r;
    memset(g_out, 0xA5, sizeof g_out);   /* unwritten slots must stay unread */
    int rc = m3u_parse(src_fn, &s, base, out_max ? g_out : NULL, out_max,
                       &g_scr, &r);
    /* fail_at == tlen fails too: the call after the last byte errors
     * instead of returning the 0 that means end of input. */
    int failed = fail_at != 0 && fail_at <= tlen;
    FUZZ_CHECK(rc == M3U_OK || (failed && rc == M3U_EIO));
    check_result(&r, out_max, tlen, g_out);
    if (failed || tlen > FUZZVOL_FILE_MAX) return 0;

    /* The same playlist off a FAT32 volume, whole: must be the same parse. */
    m3u_result_t r2;
    uint32_t     cmp = out_max ? out_max : 1u;
    fuzzvol_build("LIST    M3U", text, tlen, 0, tlen, ctl & 1u);
    fat32_t *fs = fuzzvol_mount();
    FUZZ_CHECK(fs != NULL);
    rc = m3u_parse_file(fs, fuzzvol_first_clus(), tlen, base, g_out2, cmp,
                        &g_scr, &r2);
    FUZZ_CHECK(rc == M3U_OK);
    check_result(&r2, cmp, tlen, g_out2);
    FUZZ_CHECK(r2.total == r.total);
    FUZZ_CHECK(r2.bytes == r.bytes);
    FUZZ_CHECK(r2.skipped_long == r.skipped_long);
    FUZZ_CHECK(r2.skipped_escape == r.skipped_escape);
    FUZZ_CHECK(r2.skipped_bad == r.skipped_bad);
    FUZZ_CHECK(r2.had_bom == r.had_bom && r2.had_extm3u == r.had_extm3u);
    for (uint32_t i = 0; i < r.count && i < r2.count; i++) {
        FUZZ_CHECK(strcmp(g_out[i].path, g_out2[i].path) == 0);
        FUZZ_CHECK(strcmp(g_out[i].title, g_out2[i].title) == 0);
        FUZZ_CHECK(g_out[i].duration_s == g_out2[i].duration_s);
    }
    return 0;
}
