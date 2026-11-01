/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_tag_flac.c — codecs/dr_flac/tag_flac.c, the whole-file
 * Vorbis-comment + PICTURE reader, which hands the bytes to dr_flac's
 * drflac_open_memory_with_metadata() (vendored dr_flac.h, compiled in the
 * same flac library the host build links). tag_flac.c itself is NOT in
 * core.elf (the hw build compiles dr_flac/flac.c alone and nothing calls
 * tag_flac_read today); what this harness is really for is dr_flac's own
 * metadata walk — block sizes, comment counts, PICTURE lengths, seektables —
 * which the device's streaming decoder runs at every open.
 *
 * FEEDS: the input is a complete FLAC file in memory. Same seeds as
 * fuzz_flac_meta plus a PICTURE-bearing synthetic file.
 *
 * BAD, beyond ASan/UBSan (which here also watches dr_flac's own parsing)
 * and a hang:
 *   - a return other than 0 / -1;
 *   - a field unterminated inside TAG_FIELD_MAX;
 *   - a found_* flag that is not 0/1 (found_art: 0/1/2), art without bytes,
 *     or art longer than the file it was copied out of.
 * Leaks count: libFuzzer's leak check runs, and audio_tags_free() is the
 * documented release, so a stashed picture that escapes it is a finding.
 *
 * KNOWN, upstream: dr_flac's little-endian u32 read shifts a byte into the
 * sign bit of an int (UB for a byte >= 0x80). dr_flac is vendored verbatim,
 * so it is built with -fno-sanitize=shift-base and nothing else relaxed —
 * tests/fuzz/meson.build has the details, regress/tag_flac/
 * xfail-drflac-le32-shift the input.
 */

#include "fuzz.h"

#include "../../codecs/dr_flac/tag_flac.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20)) return 0;
    audio_tags_t t;
    memset(&t, 0x5A, sizeof t);
    int rc = tag_flac_read(data, size, &t);
    FUZZ_CHECK(rc == 0 || rc == -1);
    if (rc != 0) {
        /* -1 after the memset inside means a zeroed struct with no art; -1
         * on an empty input means the struct was never touched. */
        if (size != 0) FUZZ_CHECK(t.art_bytes == NULL);
        return 0;
    }
    FUZZ_CHECK(fuzz_terminated(t.title, sizeof t.title));
    FUZZ_CHECK(fuzz_terminated(t.artist, sizeof t.artist));
    FUZZ_CHECK(fuzz_terminated(t.album, sizeof t.album));
    FUZZ_CHECK(fuzz_terminated(t.genre, sizeof t.genre));
    FUZZ_CHECK(fuzz_terminated(t.composer, sizeof t.composer));
    FUZZ_CHECK(t.found_title == 0 || t.found_title == 1);
    FUZZ_CHECK(t.found_artist == 0 || t.found_artist == 1);
    FUZZ_CHECK(t.found_album == 0 || t.found_album == 1);
    FUZZ_CHECK(t.found_genre == 0 || t.found_genre == 1);
    FUZZ_CHECK(t.found_composer == 0 || t.found_composer == 1);
    FUZZ_CHECK(t.found_art >= 0 && t.found_art <= 2);
    FUZZ_CHECK((t.found_art == 0) == (t.art_bytes == NULL));
    if (t.art_bytes) {
        FUZZ_CHECK(t.art_len > 0 && t.art_len <= size);
    }
    audio_tags_free(&t);
    return 0;
}
