/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_vol.h — a known-good in-RAM FAT32 volume holding ONE file
 * whose bytes are the fuzzer's.
 *
 * For the parsers that read a file the host tool wrote — CORECFG.DAT,
 * CORELOG.BIN, COREOTG.DAT, CORELIB.IDX — the interesting bytes are the
 * file's, not the filesystem's (fuzz_fat32.c mutates the filesystem). So the
 * volume here is fixed and correct, and only the file content, its length,
 * the size its directory entry claims, and whether its cluster chain is
 * fragmented come from the input. The geometry is the device's: 2048-byte
 * FS sectors, one sector per cluster, the partition at a non-zero LBA, the
 * same shape tests/kernel/{config,evlog,otg_store}_test.c build.
 *
 * Every write a store module issues lands in fuzzvol_write(), which holds it
 * to the contract those modules exist to keep: a whole number of physical
 * sectors, physically aligned, and EVERY sector inside the file's own
 * clusters. A write anywhere else is the user's library overwritten, so it
 * is a FUZZ_CHECK failure even though no sanitizer would notice.
 */
#ifndef CORE_TESTS_FUZZ_FUZZ_VOL_H
#define CORE_TESTS_FUZZ_FUZZ_VOL_H

#include <stddef.h>
#include <stdint.h>

#include "../../fs/fat32.h"

#define FUZZVOL_BPS       2048u
#define FUZZVOL_PART_LBA  64u
#define FUZZVOL_FS_SECS   64u                       /* clusters 2..63      */
#define FUZZVOL_FILE_CLUS 56u                       /* at most, from 3 up  */
#define FUZZVOL_FILE_MAX  (FUZZVOL_FILE_CLUS * FUZZVOL_BPS)   /* 112 KiB   */

/*
 * Build the volume. `name83` is the raw 11-byte short name ("CORECFG DAT").
 * The file holds `content[0..len)` (len is capped at FUZZVOL_FILE_MAX),
 * allocated as `alloc_clus` clusters (0 = just enough for len; capped);
 * its directory entry records `dirent_size` bytes. `fragmented` scatters the
 * chain so a run crosses non-adjacent clusters. Returns the clamped content
 * length.
 */
size_t fuzzvol_build(const char name83[11], const uint8_t *content, size_t len,
                     uint32_t alloc_clus, uint32_t dirent_size, int fragmented);

/* Mount it (through the real fat32_mount) and return the volume. The FAT32
 * reader's caches are dropped first: they are keyed by the fat32_t address,
 * which is the same static every iteration, so a stale cache would make one
 * input's result depend on the one before. NULL if the mount fails (it
 * cannot, unless fat32.c changed). */
fat32_t *fuzzvol_mount(void);

/* Fail reads that touch 512-byte LBA `lba` the next `times` times (0 = no
 * fault). The store modules retry a read error; this drives that path. */
void fuzzvol_fault(uint32_t lba, uint32_t times);

/* The raw read callback, exposed for a harness that wants it. */
int fuzzvol_read(void *ud, uint32_t lba, uint32_t count, void *buf);

/* The write callback: checks the contract above, then applies the write to
 * the image so a re-read sees it. `fail` makes it return -1 without writing
 * (a drive refusing), still after checking the address. Counts in *writes. */
int fuzzvol_write(uint32_t lba, uint32_t count, const void *buf);
void fuzzvol_write_fail(int fail);
uint32_t fuzzvol_writes(void);

/* Absolute LBA of the file's cluster `i` (0-based along the chain), and how
 * many clusters it has. */
uint32_t fuzzvol_file_lba(uint32_t i);
uint32_t fuzzvol_file_nclus(void);

/* Cluster number of the file's first cluster (for a harness that streams it
 * with fat32_stream_open), and the size its directory entry claims. */
uint32_t fuzzvol_first_clus(void);
uint32_t fuzzvol_dirent_size(void);

#endif /* CORE_TESTS_FUZZ_FUZZ_VOL_H */
