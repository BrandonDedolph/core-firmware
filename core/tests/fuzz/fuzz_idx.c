/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_idx.c — CORELIB.IDX, the host-built library index: the
 * header validation and the record CRC (library/idx.c), streamed off a
 * FAT32 volume through fat32_stream_read exactly the way kernel/main.c's
 * library_load_index() streams it.
 *
 * WHAT IS NOT COVERED, AND WHY. library_load_index() itself lives in
 * kernel/main.c, which only the ARM build compiles (it pulls in the whole
 * UI and HAL), and its record decode — field_copy(), genre_intern(),
 * album_intern(), folder_clus_h() — are statics there. This harness
 * reproduces the loader's STREAM SHAPE (12 bytes, then 4 more only when the
 * version word says 2, then 64-record batches, the CRC fed batch by batch)
 * so idx.c sees the calls it sees on the device, but the per-record field
 * copy is not exercised. Moving those helpers out of main.c (as idx.c was
 * moved) is what would let a harness reach them.
 * tests/scripts/check_index_loader.py is what pins main.c to this shape.
 *
 * FEEDS: byte 0 is a control byte, the rest is the FILE's content, placed
 * in the root of a correct in-RAM volume (fuzz_vol.c) as CORELIB.IDX.
 *   bit 0      re-sign: a v2 header's CRC is recomputed over everything
 *              after the 16-byte header, so mutated records get accepted;
 *   bit 1      scatter the cluster chain;
 *   bits 6-7   the directory entry's size: the content length / that minus
 *              one / plus 256 (a record the chain cannot supply) / 0.
 * Seeds: v1 and v2 indexes in build_index.py's record layout, 0 / 1 / 3
 * records (tests/fuzz/make_corpus.py).
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - idx_header_parse() accepting a header whose record count does not
 *     account for the file exactly (hdr_len + count * 256 == size, with no
 *     wrap) or whose hdr_len disagrees with its version — the loader sizes
 *     its whole read loop from that count;
 *   - crc32_update() disagreeing with an independent CRC-32 over the same
 *     bytes, fed in the same batches;
 *   - an accepted index whose stream then comes up short or errors on a
 *     volume that holds every byte the directory entry claims.
 */

#include "fuzz.h"

#include "../../library/idx.h"
#include "fuzz_vol.h"

static uint8_t g_file[FUZZVOL_FILE_MAX];
static uint8_t g_batch[64u * IDX_REC_SIZE];

static void check_header(const uint8_t *h, uint32_t file_size)
{
    idx_hdr_t out;
    if (idx_header_parse(h, file_size, &out) != IDX_OK) return;
    uint32_t ver = (uint32_t)h[4] | ((uint32_t)h[5] << 8);
    FUZZ_CHECK(out.hdr_len == (ver == 2 ? IDX_HDR_V2 : IDX_HDR_V1));
    FUZZ_CHECK(out.has_crc == (ver == 2));
    uint64_t want = (uint64_t)out.hdr_len + (uint64_t)out.count * IDX_REC_SIZE;
    FUZZ_CHECK(want == file_size);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1u) return 0;
    uint8_t ctl = data[0];
    size_t  len = size - 1u;
    if (len > FUZZVOL_FILE_MAX) len = FUZZVOL_FILE_MAX;
    memcpy(g_file, data + 1, len);
    /* What the volume holds past the content is the last cluster's zeroed
     * slack, and a directory entry that claims more (bits 6-7 = 2) reads it.
     * Keep g_file equal to the disk there too, so the reference CRC below is
     * over the bytes the stream really returns. */
    size_t slack = len + IDX_REC_SIZE + FUZZVOL_BPS;
    if (slack > FUZZVOL_FILE_MAX) slack = FUZZVOL_FILE_MAX;
    memset(g_file + len, 0, slack - len);
    if ((ctl & 1u) && len >= IDX_HDR_V2 && g_file[4] == 2 && g_file[5] == 0) {
        fuzz_put32(&g_file[12], fuzz_crc32(g_file + IDX_HDR_V2, len - IDX_HDR_V2));
    }

    /* The parser on its own, against a few claimed sizes, including ones
     * that tempt the count * 256 arithmetic. */
    if (len >= IDX_HDR_V2) {
        static const uint32_t sizes[] = { 0, 12, 16, 268, 0xFFFFFFFFu, 0x80000010u };
        check_header(g_file, (uint32_t)len);
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            check_header(g_file, sizes[i]);
        }
    }

    uint32_t dsize;
    switch (ctl >> 6) {
    case 0:  dsize = (uint32_t)len; break;
    case 1:  dsize = len ? (uint32_t)len - 1u : 0u; break;
    case 2:  dsize = (uint32_t)len + IDX_REC_SIZE; break;
    default: dsize = 0; break;
    }
    fuzzvol_build("CORELIB IDX", g_file, len, 0, dsize, (ctl >> 1) & 1u);
    fat32_t *fs = fuzzvol_mount();
    FUZZ_CHECK(fs != NULL);

    /* library_load_index()'s stream, step for step. */
    fat32_stream_t st;
    fat32_stream_open(&st, fs, fuzzvol_first_clus(), dsize);
    uint8_t hdr[IDX_HDR_V2];
    if (fat32_stream_read(&st, hdr, IDX_HDR_V1) != (int32_t)IDX_HDR_V1) return 0;
    if ((hdr[4] | (hdr[5] << 8)) == 2 &&
        fat32_stream_read(&st, hdr + IDX_HDR_V1, IDX_HDR_V2 - IDX_HDR_V1)
            != (int32_t)(IDX_HDR_V2 - IDX_HDR_V1)) {
        return 0;
    }
    idx_hdr_t h;
    if (idx_header_parse(hdr, dsize, &h) != IDX_OK) return 0;
    check_header(hdr, dsize);

    /* An accepted header means the directory entry's size is exactly the
     * records; the volume holds them all unless that size outruns the chain. */
    int whole = dsize <= len;
    uint32_t crc = 0xFFFFFFFFu, n = 0;
    size_t   pos = h.hdr_len;
    while (n < h.count) {
        uint32_t batch = h.count - n;
        if (batch > 64u) batch = 64u;
        int32_t got = fat32_stream_read(&st, g_batch, batch * IDX_REC_SIZE);
        if (got < 0 || (uint32_t)got < batch * IDX_REC_SIZE) {
            FUZZ_CHECK(!whole);
            return 0;
        }
        FUZZ_CHECK(memcmp(g_batch, g_file + pos, (size_t)got) == 0);
        if (h.has_crc) crc = crc32_update(crc, g_batch, batch * IDX_REC_SIZE);
        pos += (size_t)got;
        n += batch;
    }
    FUZZ_CHECK(pos == dsize);
    if (h.has_crc) {
        FUZZ_CHECK(~crc == fuzz_crc32(g_file + IDX_HDR_V2, dsize - IDX_HDR_V2));
    }
    return 0;
}
