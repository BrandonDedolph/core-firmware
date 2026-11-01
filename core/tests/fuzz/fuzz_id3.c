/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_id3.c — codecs/pvmp3/id3_meta.c, the MP3 tag reader the
 * library scan and Now Playing run on every MP3 (with mp3_frame.c for the
 * duration and meta_text.c for the text policy, as the device links them).
 *
 * FEEDS: the input is an MP3 file through a decoder_source_t positioned at
 * its start. Seeds: the head of each committed MP3 vector, plus synthetic
 * ID3v2.2/2.3/2.4 tags with every text encoding (UTF-16 with and without a
 * BOM, a surrogate pair), an extended header, unsynchronisation, a v2.4
 * footer and an APIC to skip (tests/fuzz/make_corpus.py).
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - a return other than 0 / -1, or -1 with anything left in *out (the
 *     header promises a zeroed struct, the "use the filename" signal);
 *   - 0 without have == 1, or a sample rate of 0;
 *   - a text field that is unterminated, or not the display policy's UTF-8
 *     (no C0 controls, no DEL, no torn or overlong sequence) — the renderer
 *     and the marquee trust these fields completely;
 *   - a negative track or year;
 *   - ReplayGain reported: the reader documents that it never reads it.
 */

#include "fuzz.h"

#include "../../codecs/pvmp3/id3_meta.h"

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
    memset(&out, 0x5A, sizeof out);      /* the reader must zero it itself */
    int rc = id3_meta_read(&src, &out);
    FUZZ_CHECK(rc == 0 || rc == -1);
    if (rc != 0) {
        static const flac_meta_t zero;
        FUZZ_CHECK(memcmp(&out, &zero, sizeof out) == 0);
        return 0;
    }
    FUZZ_CHECK(out.have == 1);
    FUZZ_CHECK(out.sample_rate != 0);
    check_text(out.title, sizeof out.title);
    check_text(out.artist, sizeof out.artist);
    check_text(out.album, sizeof out.album);
    check_text(out.genre, sizeof out.genre);
    FUZZ_CHECK(out.track >= 0 && out.year >= 0);
    FUZZ_CHECK(out.have_rg == 0);
    return 0;
}
