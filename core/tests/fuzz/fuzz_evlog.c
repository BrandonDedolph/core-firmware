/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_evlog.c — kernel/evlog.c: CORELOG.BIN, the on-disk event
 * log, whose header is validated and whose ring is binary-searched for the
 * write cursor at every boot, then written to.
 *
 * FEEDS: byte 0 is a control byte, byte 1 how much text to capture before
 * the flush (x 64 bytes), the rest is the FILE's content, placed in a
 * correct in-RAM FAT32 volume (fuzz_vol.c) in the root as CORELOG.BIN, one
 * 2048-byte block per cluster.
 *   bit 0          re-sign the header block (CRC-32 of bytes 0..16 at 16);
 *   bit 1          re-sign every ring block that carries the block magic, so
 *                  mutated seqs/boot ids/lengths reach the cursor search;
 *   bit 2          scatter the cluster chain;
 *   bits 3-4       a read fault: none / the header fails once / ring slot
 *                  (byte 1 % blocks) never reads / the root fails once;
 *   bit 5          the drive refuses the flush's writes;
 *   bits 6-7       the directory entry's size: the content length / that
 *                  rounded down to a block / one block more than the content
 *                  / the whole allocated chain.
 * Seeds: a fresh file, a partly written ring, a wrapped ring with a torn
 * slot (tools/make_log.py's format, from tests/fuzz/make_corpus.py).
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - evlog_probe_header_lba() / evlog_probe_lba() resolving anywhere but
 *     the block's own cluster (block b of the file is chain cluster b here),
 *     or failing without zeroing the out-param;
 *   - a flush writing anything outside the file, any block 0 write, or a
 *     write that is not one whole aligned block (fuzzvol_write + below);
 *   - a mount that reports ON with a boot id of 0, or a flush reporting
 *     WROTE with nothing written / FAILED without a refused write.
 */

#include "fuzz.h"

#include "../../kernel/evlog.h"
#include "fuzz_vol.h"

static uint8_t  g_file[FUZZVOL_FILE_MAX];
static uint32_t g_blocks_written;

/* The injected write: the recorder, plus evlog's own promises. */
static int ev_write(uint32_t lba, uint32_t count, const void *buf)
{
    FUZZ_CHECK(count == EVLOG_BLOCK_SECTORS);
    FUZZ_CHECK(lba != fuzzvol_file_lba(0));          /* never the header */
    uint32_t seq = 0, len = 0;
    uint16_t boot = 0;
    int final = 0;
    /* What it writes, it must be able to read back. */
    FUZZ_CHECK(evlog_block_decode((const uint8_t *)buf, &seq, &boot, &len, &final));
    int rc = fuzzvol_write(lba, count, buf);
    if (rc == 0) g_blocks_written++;
    return rc;
}

static int ev_wake(void) { return 0; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2u) return 0;
    uint8_t ctl  = data[0];
    uint8_t text = data[1];
    size_t  len  = size - 2u;
    if (len > FUZZVOL_FILE_MAX) len = FUZZVOL_FILE_MAX;
    memcpy(g_file, data + 2, len);

    uint32_t nblk = (uint32_t)(len / EVLOG_BLOCK_BYTES);
    if ((ctl & 1u) && len >= 20u) {
        fuzz_put32(&g_file[16], fuzz_crc32(g_file, 16));
    }
    if (ctl & 2u) {
        for (uint32_t b = 1; b < nblk; b++) {
            uint8_t *blk = &g_file[(size_t)b * EVLOG_BLOCK_BYTES];
            if (memcmp(blk, "CLOB", 4) != 0) continue;
            /* [0,12) ++ [16,2048), the same coverage make_log.py signs */
            uint8_t tmp[EVLOG_BLOCK_BYTES - 4u];
            memcpy(tmp, blk, 12);
            memcpy(tmp + 12, blk + 16, EVLOG_BLOCK_BYTES - 16u);
            fuzz_put32(&blk[12], fuzz_crc32(tmp, sizeof tmp));
        }
    }

    uint32_t chain = (uint32_t)((len + EVLOG_BLOCK_BYTES - 1u) / EVLOG_BLOCK_BYTES);
    uint32_t dsize;
    switch (ctl >> 6) {
    case 0:  dsize = (uint32_t)len; break;
    case 1:  dsize = nblk * EVLOG_BLOCK_BYTES; break;
    case 2:  dsize = (uint32_t)len + EVLOG_BLOCK_BYTES; break;
    default: dsize = (chain ? chain : 1u) * EVLOG_BLOCK_BYTES; break;
    }
    fuzzvol_build("CORELOG BIN", g_file, len, 0, dsize, (ctl >> 2) & 1u);
    fat32_t *fs = fuzzvol_mount();
    FUZZ_CHECK(fs != NULL);

    uint32_t fault = (ctl >> 3) & 3u;
    uint32_t nclus = fuzzvol_file_nclus();
    if (fault == 1) fuzzvol_fault(fuzzvol_file_lba(0), 1);
    if (fault == 2) fuzzvol_fault(fuzzvol_file_lba(text % nclus), 1000);
    if (fault == 3) fuzzvol_fault(FUZZVOL_PART_LBA + 2u * (FUZZVOL_BPS / 512u), 1);
    g_blocks_written = 0;

    int on = evlog_mount(fs, ev_write, ev_wake);
    FUZZ_CHECK(on == evlog_enabled());
    uint32_t lba = 0xDEADu;
    if (evlog_probe_header_lba(&lba) == 0) {
        FUZZ_CHECK(lba == fuzzvol_file_lba(0));
    } else {
        FUZZ_CHECK(lba == 0);
    }
    if (!on) return 0;
    FUZZ_CHECK(evlog_boot_id() != 0);
    FUZZ_CHECK(evlog_prev_final() >= -1 && evlog_prev_final() <= 1);

    for (uint32_t k = 0; k < 3u; k++) {
        uint32_t seq = evlog_seq() + k;
        lba = 0xDEADu;
        if (evlog_probe_lba(seq, &lba) == 0) {
            uint32_t blk = (uint32_t)evlog_block_index(seq, dsize / EVLOG_BLOCK_BYTES);
            FUZZ_CHECK(blk != 0 && blk < nclus);
            FUZZ_CHECK(lba == fuzzvol_file_lba(blk));
        } else {
            FUZZ_CHECK(lba == 0);
        }
    }

    /* Something to say, then a forced flush (suspend's), which drains. */
    for (uint32_t i = 0; i < (uint32_t)text * 64u; i++) {
        evlog_capture((uint8_t)('a' + i % 26u));
    }
    evlog_capture('\n');
    int refuse = (ctl >> 5) & 1u;
    fuzzvol_write_fail(refuse);
    cfg_commit_env_t env = { 1000000u, 0, 0, 1, 1 };
    uint32_t w0 = fuzzvol_writes();
    int fr = evlog_flush(CFG_COMMIT_FORCE, &env);
    uint32_t attempts = fuzzvol_writes() - w0;
    FUZZ_CHECK(attempts <= EVLOG_FORCE_MAX_BLOCKS);
    if (fr == EVLOG_FLUSH_WROTE)  FUZZ_CHECK(g_blocks_written > 0);
    if (fr == EVLOG_FLUSH_FAILED) FUZZ_CHECK(refuse || attempts == 0);
    fuzzvol_write_fail(0);

    /* Drain whatever is left so the next input starts from an empty ring
     * (the ring is module state the mount deliberately keeps). */
    for (int i = 0; i < 4 && evlog_pending() != 0 && evlog_enabled(); i++) {
        env.now_us += 4000000u;
        (void)evlog_flush(CFG_COMMIT_FORCE, &env);
    }
    return 0;
}
