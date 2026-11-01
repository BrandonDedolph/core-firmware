/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_config.c — kernel/config.c: CORECFG.DAT, the settings
 * record, read at every boot from whatever the disk holds.
 *
 * FEEDS: byte 0 is a control byte, the rest is the FILE's content, placed in
 * a correct in-RAM FAT32 volume (fuzz_vol.c) in the root as CORECFG.DAT.
 *   bit 0 / bit 1  re-sign slot 0 / slot 1 (recompute the CRC-32 at 1020 of
 *                  that 1024-byte record) so a mutation reaches the field
 *                  decoder instead of dying at the CRC;
 *   bit 2          scatter the cluster chain;
 *   bits 3-4       a read fault: none / slot 0 fails once / slot 1 never
 *                  reads / the root directory fails once (the boot retries);
 *   bit 5          the save's write is refused by the drive;
 *   bits 6-7       what the directory entry claims the size is: the content
 *                  length / 2048 / 2047 / 64 KiB.
 * Seeds: tools/make_config.py --emit's golden record (both slots) and a
 * record per payload length the format has had (12, 24, 44, 48, 64, 68).
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - config_decode() of a record it accepted not being a fixed point: the
 *     settings it produced, re-encoded, must decode to the same settings —
 *     the header promises every field is clamped, so nothing CRC-valid can
 *     install a value config_encode would not write;
 *   - config_probe_lba() resolving a slot anywhere but its place in the
 *     file's FIRST cluster (slot * 2 LBAs in), or config_save() writing
 *     outside the file at all (fuzzvol_write's FUZZ_CHECKs);
 *   - after a save that succeeded, the next config_load() not finding that
 *     record with seq + 1; after one the drive refused, the old record not
 *     still winning — the two-slot scheme's whole argument.
 */

#include "fuzz.h"

#include "../../kernel/config.h"
#include "fuzz_vol.h"

/* The HAL: config_save() calls ata_write_sectors directly. */
int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf)
{
    return fuzzvol_write(lba, count, buf);
}

static uint8_t g_file[FUZZVOL_FILE_MAX];
static uint8_t g_rec[CONFIG_SLOT_BYTES] __attribute__((aligned(4)));
static uint8_t g_rec2[CONFIG_SLOT_BYTES] __attribute__((aligned(4)));

/* decode -> encode -> decode is the identity on anything decode accepts. */
static void check_fixed_point(const settings_t *s, uint32_t seq)
{
    config_encode(g_rec, s, seq);
    settings_t s2 = *s;
    uint32_t seq2 = 0;
    FUZZ_CHECK(config_decode(g_rec, &s2, &seq2) == 1);
    FUZZ_CHECK(seq2 == seq);
    config_encode(g_rec2, &s2, seq2);
    FUZZ_CHECK(memcmp(g_rec, g_rec2, sizeof g_rec) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1u) return 0;
    uint8_t ctl = data[0];
    size_t  len = size - 1u;
    if (len > FUZZVOL_FILE_MAX) len = FUZZVOL_FILE_MAX;
    memcpy(g_file, data + 1, len);

    for (uint32_t slot = 0; slot < CONFIG_SLOTS; slot++) {
        size_t off = (size_t)slot * CONFIG_SLOT_BYTES;
        if ((ctl >> slot) & 1u && len >= off + CONFIG_SLOT_BYTES) {
            fuzz_put32(&g_file[off + CONFIG_SLOT_BYTES - 4u],
                       fuzz_crc32(&g_file[off], CONFIG_SLOT_BYTES - 4u));
        }
    }

    /* The codec alone, on each whole record the content holds. */
    for (size_t off = 0; off + CONFIG_SLOT_BYTES <= len && off < 2u * CONFIG_SLOT_BYTES;
         off += CONFIG_SLOT_BYTES) {
        memcpy(g_rec, &g_file[off], CONFIG_SLOT_BYTES);
        settings_t s;
        settings_defaults(&s);
        uint32_t seq = 0;
        if (config_decode(g_rec, &s, &seq)) check_fixed_point(&s, seq);
    }

    /* The load / save cycle off a volume. */
    static const uint32_t sizes[4] = { 0, 2048u, 2047u, 65536u };
    uint32_t dsize = (ctl >> 6) ? sizes[ctl >> 6] : (uint32_t)len;
    fuzzvol_build("CORECFG DAT", g_file, len, 0, dsize, (ctl >> 2) & 1u);
    fat32_t *fs = fuzzvol_mount();
    FUZZ_CHECK(fs != NULL);

    uint32_t fault = (ctl >> 3) & 3u;
    uint32_t base  = fuzzvol_file_lba(0);
    if (fault == 1) fuzzvol_fault(base, 1);
    if (fault == 2) fuzzvol_fault(base + CONFIG_SLOT_SECTORS, 1000);
    if (fault == 3) fuzzvol_fault(FUZZVOL_PART_LBA + 2u * (FUZZVOL_BPS / 512u), 1);

    settings_t s;
    settings_defaults(&s);
    int got = config_load(fs, &s);
    FUZZ_CHECK(got == 0 || got == 1);
    if (got) {
        FUZZ_CHECK(config_writable());
        check_fixed_point(&s, config_seq());
    }
    for (uint32_t slot = 0; slot < CONFIG_SLOTS; slot++) {
        uint32_t lba = 0xDEADu;
        if (config_probe_lba(slot, &lba) == 0) {
            FUZZ_CHECK(config_writable());
            FUZZ_CHECK(lba == base + slot * CONFIG_SLOT_SECTORS);
        } else {
            FUZZ_CHECK(lba == 0);
        }
    }
    if (!config_writable()) {
        FUZZ_CHECK(config_save(&s) != 0);
        FUZZ_CHECK(fuzzvol_writes() == 0);
        return 0;
    }

    uint32_t seq0 = config_seq();
    int refuse = (ctl >> 5) & 1u;
    fuzzvol_write_fail(refuse);
    int rc = config_save(&s);
    FUZZ_CHECK(fuzzvol_writes() <= 1u);
    if (fault != 0) return 0;            /* a stale unreadable slot muddies it */
    fuzzvol_write_fail(0);

    settings_t again;
    settings_defaults(&again);
    int got2 = config_load(fs, &again);
    if (rc == 0) {
        FUZZ_CHECK(!refuse);
        FUZZ_CHECK(got2 == 1);
        FUZZ_CHECK(config_seq() == seq0 + 1u);
        config_encode(g_rec, &s, 0);
        config_encode(g_rec2, &again, 0);
        FUZZ_CHECK(memcmp(g_rec, g_rec2, sizeof g_rec) == 0);
    } else if (got) {
        FUZZ_CHECK(got2 == 1 && config_seq() == seq0);
    }
    return 0;
}
