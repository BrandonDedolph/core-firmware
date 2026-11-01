/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_flac_meta.c — codecs/flac_meta.c, the FLAC metadata reader
 * (STREAMINFO, SEEKTABLE count, VORBIS_COMMENT incl. ReplayGain), with
 * meta_text.c for the text policy, as the device links them.
 *
 * FEEDS: the input is a FLAC file through a decoder_source_t at its start.
 * Seeds: the metadata head of each committed FLAC vector, plus a synthetic
 * file with a full tag set, ReplayGain gains and peaks in every spelling the
 * parser accepts, a SEEKTABLE, PADDING and a PICTURE to skip
 * (tests/fuzz/make_corpus.py).
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - a return other than 0 / -1; 0 without have == 1; -1 with anything left
 *     in *out (the header's "out is left zeroed" promise);
 *   - a text field unterminated or not the display policy's UTF-8;
 *   - a negative track or year, have_rg bits outside FLAC_META_RG_*, a gain
 *     or peak value present without its have_rg bit (0 is what "absent"
 *     means — the player's clip cap reads the bit, not the value);
 *   - a duration reported with no sample rate to have computed it from.
 */

#include "fuzz.h"

#include "../../codecs/flac_meta.h"

static void check_text(const char *s, size_t cap)
{
    FUZZ_CHECK(fuzz_terminated(s, cap));
    FUZZ_CHECK(fuzz_utf8_ok(s, 0));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20)) return 0;
    fuzz_mem_t m = { data, size, 0, 0 };
    decoder_source_t src = { fuzz_mem_read, fuzz_mem_seek, fuzz_mem_tell, &m };
    flac_meta_t out;
    memset(&out, 0x5A, sizeof out);
    int rc = flac_meta_read(&src, &out);
    FUZZ_CHECK(rc == 0 || rc == -1);
    if (rc != 0) {
        static const flac_meta_t zero;
        FUZZ_CHECK(memcmp(&out, &zero, sizeof out) == 0);
        return 0;
    }
    FUZZ_CHECK(out.have == 1);
    check_text(out.title, sizeof out.title);
    check_text(out.artist, sizeof out.artist);
    check_text(out.album, sizeof out.album);
    check_text(out.genre, sizeof out.genre);
    FUZZ_CHECK(out.track >= 0 && out.year >= 0);
    FUZZ_CHECK(out.duration_s == 0 || out.sample_rate != 0);

    const int all = (int)(FLAC_META_RG_TRACK | FLAC_META_RG_ALBUM |
                          FLAC_META_RG_TRACK_PEAK | FLAC_META_RG_ALBUM_PEAK);
    FUZZ_CHECK((out.have_rg & ~all) == 0);
    if (!(out.have_rg & (int)FLAC_META_RG_TRACK))      FUZZ_CHECK(out.rg_track_q8 == 0);
    if (!(out.have_rg & (int)FLAC_META_RG_ALBUM))      FUZZ_CHECK(out.rg_album_q8 == 0);
    if (!(out.have_rg & (int)FLAC_META_RG_TRACK_PEAK)) FUZZ_CHECK(out.rg_track_peak_q16 == 0);
    if (!(out.have_rg & (int)FLAC_META_RG_ALBUM_PEAK)) FUZZ_CHECK(out.rg_album_peak_q16 == 0);
    return 0;
}
