/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_otg_store.c — kernel/otg_store.c: COREOTG.DAT, the
 * On-The-Go live list, two 5120-byte slots that straddle cluster boundaries
 * on the device's geometry, read at boot and rewritten in place.
 *
 * FEEDS: byte 0 is a control byte, the rest is the FILE's content, placed in
 * a correct in-RAM FAT32 volume (fuzz_vol.c) in the root as COREOTG.DAT.
 *   bit 0 / bit 1  re-sign slot 0 / slot 1 (CRC-32 of [0, 5116) at 5116);
 *   bit 2          scatter the cluster chain (every slot is then several
 *                  non-adjacent runs, and slot 1 starts mid-cluster);
 *   bits 3-4       a read fault: none / slot 0's first run fails once /
 *                  slot 1's first run never reads / the root fails once;
 *   bit 5          the drive refuses the save;
 *   bits 6-7       the directory entry's size: the content length / 10240 /
 *                  10239 / the whole allocated chain.
 * Seeds: tools/make_otg.py --emit's golden file, and slots at 0, 1 and 512
 * entries, a wrapped seq pair and a torn slot (tests/fuzz/make_corpus.py).
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - a decoded or mounted list with n > OTG_MAX or a stored (0, 0) pair
 *     (the header: a null pair inside the count is dropped, never stored);
 *   - otg_slot_decode() not being a fixed point: encode what it produced,
 *     decode again, the same list must come back;
 *   - a mount returning 0 with a non-empty list (0 means otg_init()'d);
 *   - otg_store_probe_lba() resolving a slot anywhere but its own byte
 *     offset's sector in the chain, or the save writing outside the file or
 *     across a cluster it did not resolve (fuzzvol_write's FUZZ_CHECKs);
 *   - after a successful save, a remount not reading back that list with
 *     seq + 1; after a refused one, the old slot not still winning.
 */

#include "fuzz.h"

#include "../../kernel/otg_store.h"
#include "fuzz_vol.h"

static uint8_t    g_file[FUZZVOL_FILE_MAX];
static uint8_t    g_slot[OTG_SLOT_BYTES] __attribute__((aligned(4)));
static otg_list_t g_l, g_l2;

static void check_list(const otg_list_t *l)
{
    FUZZ_CHECK(l->n <= OTG_MAX);
    for (uint32_t i = 0; i < l->n; i++) {
        FUZZ_CHECK(l->e[i].folder_hash != 0 || l->e[i].file_hash != 0);
    }
}

static int lists_equal(const otg_list_t *a, const otg_list_t *b)
{
    if (a->n != b->n || a->gen != b->gen) return 0;
    return memcmp(a->e, b->e, (size_t)a->n * sizeof a->e[0]) == 0;
}

static int otg_wake(void) { return 0; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1u) return 0;
    uint8_t ctl = data[0];
    size_t  len = size - 1u;
    if (len > FUZZVOL_FILE_MAX) len = FUZZVOL_FILE_MAX;
    memcpy(g_file, data + 1, len);

    for (uint32_t slot = 0; slot < OTG_SLOTS_ON_DISK; slot++) {
        size_t off = (size_t)slot * OTG_SLOT_BYTES;
        if ((ctl >> slot) & 1u && len >= off + OTG_SLOT_BYTES) {
            fuzz_put32(&g_file[off + OTG_SLOT_BYTES - 4u],
                       fuzz_crc32(&g_file[off], OTG_SLOT_BYTES - 4u));
        }
    }

    /* The codec alone. */
    for (size_t off = 0; off + OTG_SLOT_BYTES <= len && off < 2u * OTG_SLOT_BYTES;
         off += OTG_SLOT_BYTES) {
        memcpy(g_slot, &g_file[off], OTG_SLOT_BYTES);
        uint32_t seq = 0;
        if (!otg_slot_decode(g_slot, &g_l, &seq)) continue;
        check_list(&g_l);
        otg_slot_encode(g_slot, &g_l, seq);
        uint32_t seq2 = 0;
        FUZZ_CHECK(otg_slot_decode(g_slot, &g_l2, &seq2) == 1);
        FUZZ_CHECK(seq2 == seq && lists_equal(&g_l, &g_l2));
    }

    /* Mount / save off a volume. */
    uint32_t chain = (uint32_t)((len + FUZZVOL_BPS - 1u) / FUZZVOL_BPS);
    uint32_t dsize;
    switch (ctl >> 6) {
    case 0:  dsize = (uint32_t)len; break;
    case 1:  dsize = OTG_STORE_MIN_BYTES; break;
    case 2:  dsize = OTG_STORE_MIN_BYTES - 1u; break;
    default: dsize = (chain ? chain : 1u) * FUZZVOL_BPS; break;
    }
    fuzzvol_build("COREOTG DAT", g_file, len, 0, dsize, (ctl >> 2) & 1u);
    fat32_t *fs = fuzzvol_mount();
    FUZZ_CHECK(fs != NULL);

    uint32_t fault = (ctl >> 3) & 3u;
    if (fault == 1) fuzzvol_fault(fuzzvol_file_lba(0), 1);
    if (fault == 2 && fuzzvol_file_nclus() > 2u) {
        /* slot 1 begins 5120 bytes in: cluster 2, sector 2 of it */
        fuzzvol_fault(fuzzvol_file_lba(2) + 2u, 1000);
    }
    if (fault == 3) fuzzvol_fault(FUZZVOL_PART_LBA + 2u * (FUZZVOL_BPS / 512u), 1);

    int got = otg_store_mount(fs, fuzzvol_write, otg_wake, &g_l);
    FUZZ_CHECK(got == 0 || got == 1);
    check_list(&g_l);
    if (!got) FUZZ_CHECK(g_l.n == 0);

    for (uint32_t slot = 0; slot < OTG_SLOTS_ON_DISK; slot++) {
        uint32_t lba = 0xDEADu;
        if (otg_store_probe_lba(slot, &lba) == 0) {
            uint32_t off = slot * OTG_SLOT_BYTES;
            FUZZ_CHECK(off / FUZZVOL_BPS < fuzzvol_file_nclus());
            FUZZ_CHECK(lba == fuzzvol_file_lba(off / FUZZVOL_BPS) +
                              (off % FUZZVOL_BPS) / 512u);
        } else {
            FUZZ_CHECK(lba == 0);
        }
    }
    if (!otg_store_writable()) {
        FUZZ_CHECK(otg_store_save(&g_l) != 0);
        FUZZ_CHECK(fuzzvol_writes() == 0);
        return 0;
    }

    /* Change the list (an add, as hold-Select does) and save it. */
    (void)otg_add(&g_l, 0x12345678u, (uint32_t)len | 1u);
    uint32_t seq0 = otg_store_seq();
    int refuse = (ctl >> 5) & 1u;
    fuzzvol_write_fail(refuse);
    int rc = otg_store_save(&g_l);
    if (fault != 0) return 0;
    fuzzvol_write_fail(0);

    int got2 = otg_store_mount(fs, fuzzvol_write, otg_wake, &g_l2);
    if (rc == 0) {
        FUZZ_CHECK(!refuse);
        FUZZ_CHECK(got2 == 1);
        FUZZ_CHECK(otg_store_seq() == seq0 + 1u);
        FUZZ_CHECK(lists_equal(&g_l, &g_l2));
    } else if (got) {
        FUZZ_CHECK(got2 == 1 && otg_store_seq() == seq0);
    }
    return 0;
}
