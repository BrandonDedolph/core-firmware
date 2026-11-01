/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_vol.c — see fuzz_vol.h.
 *
 * Layout (FS sectors of 2048 bytes, one per cluster, so cluster N is FS
 * sector N and absolute LBA FUZZVOL_PART_LBA + 4N):
 *   0      boot sector       1      the FAT (one sector: 512 entries)
 *   2      root directory    3..    the file, contiguous or scattered
 */

#include "fuzz_vol.h"

#include <string.h>

#include "fuzz.h"
#include "hw/ata.h"          /* ATA_PHYS_LOG: the write quantum */

#define SEC_LBAS   (FUZZVOL_BPS / 512u)
#define TOT_LBA    (FUZZVOL_PART_LBA + FUZZVOL_FS_SECS * SEC_LBAS)

static uint8_t  g_disk[TOT_LBA * 512u];
static uint32_t g_chain[FUZZVOL_FILE_CLUS];
static uint32_t g_nclus;
static uint32_t g_dirent_size;
static uint32_t g_fault_lba, g_fault_left;
static int      g_write_fail;
static uint32_t g_writes;
static fat32_t  g_fs;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t *fs_sec(uint32_t s)
{
    return &g_disk[FUZZVOL_PART_LBA * 512u + s * FUZZVOL_BPS];
}

static void put_dirent(uint8_t *e, const char *raw, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[11] = attr;
    put16(&e[20], clus >> 16);
    put16(&e[26], clus & 0xFFFFu);
    put32(&e[28], size);
}

size_t fuzzvol_build(const char name83[11], const uint8_t *content, size_t len,
                     uint32_t alloc_clus, uint32_t dirent_size, int fragmented)
{
    if (len > FUZZVOL_FILE_MAX) len = FUZZVOL_FILE_MAX;
    uint32_t need = (uint32_t)((len + FUZZVOL_BPS - 1u) / FUZZVOL_BPS);
    if (alloc_clus < need) alloc_clus = need;
    if (alloc_clus == 0) alloc_clus = 1;      /* a file always has a cluster */
    if (alloc_clus > FUZZVOL_FILE_CLUS) alloc_clus = FUZZVOL_FILE_CLUS;

    memset(g_disk, 0, sizeof g_disk);
    g_nclus        = alloc_clus;
    g_dirent_size  = dirent_size;
    g_fault_lba    = 0;
    g_fault_left   = 0;
    g_write_fail   = 0;
    g_writes       = 0;

    /* The chain. Scattered = every other cluster first, then the gaps, so
     * consecutive file clusters are never adjacent on the disk and nearly
     * every run boundary is a real discontinuity. */
    uint32_t half = (alloc_clus + 1u) / 2u;
    for (uint32_t i = 0; i < alloc_clus; i++) {
        if (!fragmented)   g_chain[i] = 3u + i;
        else if (i < half) g_chain[i] = 3u + 2u * i;
        else               g_chain[i] = 4u + 2u * (i - half);
    }

    uint8_t *bs = fs_sec(0);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], FUZZVOL_BPS);
    bs[13] = 1;                 /* sectors per cluster */
    put16(&bs[14], 1);          /* reserved sectors    */
    bs[16] = 1;                 /* one FAT             */
    bs[21] = 0xF8;
    put32(&bs[32], FUZZVOL_FS_SECS);
    put32(&bs[36], 1);          /* FAT size, sectors   */
    put32(&bs[44], 2);          /* root cluster        */
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fat = fs_sec(1);
    put32(&fat[0], 0x0FFFFFF8u);
    put32(&fat[4], 0x0FFFFFFFu);
    put32(&fat[8], 0x0FFFFFFFu);               /* root: one cluster */
    for (uint32_t i = 0; i < alloc_clus; i++) {
        put32(&fat[g_chain[i] * 4u],
              (i + 1u < alloc_clus) ? g_chain[i + 1u] : 0x0FFFFFFFu);
    }

    for (uint32_t i = 0; i < alloc_clus; i++) {
        size_t off = (size_t)i * FUZZVOL_BPS;
        if (off >= len) break;
        size_t n = len - off;
        if (n > FUZZVOL_BPS) n = FUZZVOL_BPS;
        memcpy(fs_sec(g_chain[i]), content + off, n);
    }

    /* A neighbour first, so a root walk has to look past an entry, then the
     * file. The neighbour points at the root's own cluster: harmless to a
     * reader that never opens it, and outside the file, so a write the
     * recorder below refuses. */
    uint8_t *root = fs_sec(2);
    put_dirent(&root[0], "README  TXT", 0x20, 2, 16);
    put_dirent(&root[32], name83, 0x20, g_chain[0], dirent_size);
    return len;
}

fat32_t *fuzzvol_mount(void)
{
    fat32_cache_drop(NULL);
    memset(&g_fs, 0, sizeof g_fs);
    if (fat32_mount(&g_fs, fuzzvol_read, NULL, FUZZVOL_PART_LBA) != 0) {
        return NULL;
    }
    return &g_fs;
}

void fuzzvol_fault(uint32_t lba, uint32_t times)
{
    g_fault_lba  = lba;
    g_fault_left = times;
}

int fuzzvol_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if (count == 0 || lba >= TOT_LBA || count > TOT_LBA - lba) {
        return -1;
    }
    if (g_fault_left && g_fault_lba >= lba && g_fault_lba - lba < count) {
        g_fault_left--;
        return -1;
    }
    memcpy(buf, &g_disk[(size_t)lba * 512u], (size_t)count * 512u);
    return 0;
}

/* Is absolute LBA `lba` inside one of the file's clusters? */
static int in_file(uint32_t lba)
{
    for (uint32_t i = 0; i < g_nclus; i++) {
        uint32_t base = fuzzvol_file_lba(i);
        if (lba >= base && lba < base + SEC_LBAS) return 1;
    }
    return 0;
}

int fuzzvol_write(uint32_t lba, uint32_t count, const void *buf)
{
    g_writes++;
    FUZZ_CHECK(buf != NULL);
    FUZZ_CHECK(count != 0);
    FUZZ_CHECK(count % ATA_PHYS_LOG == 0);
    FUZZ_CHECK(lba % ATA_PHYS_LOG == 0);
    FUZZ_CHECK(lba < TOT_LBA && count <= TOT_LBA - lba);
    for (uint32_t k = 0; k < count; k++) {
        FUZZ_CHECK(in_file(lba + k));
    }
    if (g_write_fail) return -1;
    memcpy(&g_disk[(size_t)lba * 512u], buf, (size_t)count * 512u);
    return 0;
}

void fuzzvol_write_fail(int fail) { g_write_fail = fail; }
uint32_t fuzzvol_writes(void)     { return g_writes; }

uint32_t fuzzvol_file_lba(uint32_t i)
{
    return FUZZVOL_PART_LBA + g_chain[i] * SEC_LBAS;
}

uint32_t fuzzvol_file_nclus(void)   { return g_nclus; }
uint32_t fuzzvol_first_clus(void)   { return g_chain[0]; }
uint32_t fuzzvol_dirent_size(void)  { return g_dirent_size; }
