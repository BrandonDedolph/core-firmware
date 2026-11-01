/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_mp3_frame.c — codecs/pvmp3/mp3_frame.c, the MPEG framing
 * layer: frame headers, the ID3v2 prefix length, the ID3v1/APE tail, the
 * Xing/Info/VBRI header and the seek map.
 *
 * FEEDS: the input is an MP3 file (seeds: the first few KB of the committed
 * codec vectors, which cover CBR, VBR with a Xing TOC and MPEG-2 mono). The
 * pure functions get the buffer directly; mp3_stream_scan() gets it through
 * a decoder_source_t, the way the player opens a file.
 *
 * BAD, beyond ASan/UBSan and a hang:
 *   - a header accepted with a frame length outside [header + side info,
 *     MP3_MAX_FRAME_BYTES], a rate the DAC table lacks, spf not 1152/576,
 *     or a channel count not 1/2 (the wrapper sizes buffers from these);
 *   - mp3_tail_tag_len() claiming more bytes than it was given;
 *   - a scan whose audio region is not inside the file and ordered
 *     seek_start <= audio_start <= audio_end, or whose duration exceeds the
 *     raw frames it was trimmed from;
 *   - mp3_seek_offset() outside [audio_start, audio_end) — the header's
 *     promise, which the decoder resyncs from without re-checking.
 */

#include "fuzz.h"

#include "../../codecs/pvmp3/mp3_frame.h"

static void check_header(const mp3_header_t *h)
{
    static const uint32_t rates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
    };
    int rate_ok = 0;
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        if (h->sample_rate == rates[i]) rate_ok = 1;
    }
    FUZZ_CHECK(rate_ok);
    FUZZ_CHECK(h->spf == 1152u || h->spf == 576u);
    FUZZ_CHECK(h->channels == 1u || h->channels == 2u);
    FUZZ_CHECK(h->version <= MP3_MPEG_1);
    FUZZ_CHECK(h->bitrate > 0u);
    FUZZ_CHECK(h->frame_bytes <= MP3_MAX_FRAME_BYTES);
    FUZZ_CHECK(h->frame_bytes >= 4u + (uint32_t)mp3_side_info_bytes(h));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20)) return 0;

    /* Every 4-byte window of the head, as a resync would try them. */
    for (size_t i = 0; i + 4u <= size && i < 64u; i++) {
        mp3_header_t h;
        if (mp3_header_parse(data + i, &h)) {
            check_header(&h);
            mp3_vbr_tag_t v;
            memset(&v, 0, sizeof v);
            int got = mp3_vbr_tag_parse(data + i, size - i, &h, &v);
            FUZZ_CHECK(got == 0 || got == 1);
            if (v.have_toc) FUZZ_CHECK(got == 1 && v.have);
        }
    }
    if (size >= 10u) (void)mp3_id3v2_tag_len(data);
    FUZZ_CHECK(mp3_tail_tag_len(data, size) <= size);

    fuzz_mem_t m = { data, size, 0, 0 };
    decoder_source_t src = { fuzz_mem_read, fuzz_mem_seek, fuzz_mem_tell, &m };
    mp3_stream_info_t si;
    if (mp3_stream_scan(&src, &si) != 0) return 0;

    check_header(&si.first);
    FUZZ_CHECK(si.audio_end <= size);
    FUZZ_CHECK(si.seek_start <= si.audio_start);
    FUZZ_CHECK(si.audio_start <= si.audio_end);
    FUZZ_CHECK(si.audio_bytes <= si.audio_end - si.seek_start);
    FUZZ_CHECK(si.total_frames <= si.raw_frames);
    FUZZ_CHECK((uint64_t)m.pos == si.audio_start);   /* left where decode starts */

    if (si.audio_end > si.audio_start) {
        uint64_t targets[6] = {
            0, 1, si.total_frames / 2u, si.total_frames, si.raw_frames + 1u,
            ~(uint64_t)0 >> 8,
        };
        for (int i = 0; i < 6; i++) {
            uint64_t off = mp3_seek_offset(&si, targets[i]);
            FUZZ_CHECK(off >= si.audio_start && off < si.audio_end);
        }
    }
    return 0;
}
