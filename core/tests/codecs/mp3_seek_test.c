/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/mp3_seek_test.c — seeking a VBR MP3 by its Xing TOC.
 *
 * MP3 has no seek table. The target second is mapped to a byte offset (the
 * Xing TOC interpolated, or proportional by byte), the decoder backs off a
 * fixed 512 bytes plus four maximum-size frames, resyncs to a real frame, and
 * decodes and discards every frame up to the landing so the bit reservoir is
 * filled before anything is heard. Whether that lands where it claims cannot
 * be checked by arithmetic agreeing with itself, so this suite asks the
 * audio: it seeks, decodes a slice, and finds where that slice actually sits
 * in ffmpeg's decode of the same file by correlation.
 *
 * The accuracy bound is one frame (26 ms) plus the TOC's granularity, which
 * is one percent of the file — the same class of answer dr_flac gives when it
 * binary-searches to a frame. Tight enough that resume lands within a second
 * on anything album-length, and loose enough not to pin an implementation
 * detail.
 *
 * Three vectors: the 44.1 kHz V5 encode, a 48 kHz V2 encode (the TOC and the
 * frame maths at another rate), and that same 48 kHz stream with its Xing
 * frame rewritten as a Fraunhofer VBRI frame. VBRI's own TOC is not used
 * (codecs/pvmp3/mp3_frame.c), so that file seeks proportionally by byte —
 * and is held to the SAME bound, because "proportional by byte" on a VBR
 * file is only as good as the bitrate is even, and the bound is the promise
 * the resume path makes either way.
 */

#include "mp3_vectors.h"

static int g_fails;

static void xpect(const char *what, int cond)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        g_fails++;
    }
}

/* Frames of audio decoded at each landing before correlating. Long enough to
 * be unambiguous in orchestral music, short enough to stay quick. */
#define PROBE_FRAMES 8192
/* How far around the expected position the correlation looks. Comfortably
 * more than the tolerance, so a miss shows up as a large error rather than
 * as "not found". */
#define SEARCH_SPAN  8000

static int seek_case(const char *dir, const char *stem, const char *ref_stem,
                     unsigned rate)
{
    char p_mp3[1024], p_ref[1024];
    snprintf(p_mp3, sizeof p_mp3, "%s/%s.mp3", dir, stem);
    snprintf(p_ref, sizeof p_ref, "%s/%s.ffmpeg.flac", dir, ref_stem);
    printf("--- %s\n", stem);

    size_t   mp3_len = 0;
    uint8_t *mp3 = mp3v_slurp(p_mp3, &mp3_len);
    mp3v_pcm_t ref = {0}, whole = {0};
    if (!mp3 || !mp3v_load_ref(p_ref, &ref) ||
        !mp3v_decode_mp3(mp3, mp3_len, &whole)) {
        fprintf(stderr, "cannot load %s\n", stem);
        free(mp3);
        mp3v_free(&ref);
        mp3v_free(&whole);
        return 0;
    }
    unsigned ch = whole.channels;

    /*
     * The fixed offset between our timeline and the reference's: ffmpeg trims
     * the LAME encoder delay from the head of its output and we do not. Every
     * landing below is measured against this, so it has to be established
     * first from a straight decode of the whole file.
     */
    long base = mp3v_best_offset(whole.pcm, whole.frames, ref.pcm, ref.frames,
                                 ch, 4096, 8192, NULL);
    xpect("the straight decode aligns to the reference", base >= 0);
    if (base < 0) return 0;
    printf("encoder-delay offset: %ld frames\n", base);

    const decoder_ops_t *ops = mp3_decoder_ops();
    decoder_t d = {0};
    xpect("the VBR vector opens", ops->open(&d, mp3, mp3_len, NULL) == DECODER_OK);
    if (!d.ops) return 0;
    xpect("the Xing/VBRI frame count gives a length", d.total_frames > 0);
    xpect("...of about two seconds",
          d.total_frames > rate * 3u / 2u && d.total_frames < rate * 5u / 2u);
    xpect("...at the vector's rate", d.sample_rate == rate);

    /* One frame plus one percent of the file, which is what the TOC resolves
     * to. Stated in frames so the number in the message is the one that
     * matters. */
    long tol = 1152 + (long)(d.total_frames / 100u);
    printf("landing tolerance: %ld frames (%.0f ms)\n",
           tol, 1000.0 * (double)tol / (double)rate);

    static const double targets_s[] = { 0.0, 0.5, 1.0, 1.5 };
    static int16_t probe[PROBE_FRAMES * 2];

    for (size_t i = 0; i < sizeof targets_s / sizeof targets_s[0]; i++) {
        uint64_t target = (uint64_t)(targets_s[i] * (double)rate);
        char     what[128];

        snprintf(what, sizeof what, "seek to %.1f s succeeds", targets_s[i]);
        xpect(what, ops->seek(&d, target) == DECODER_OK);

        size_t got = 0;
        while (got < PROBE_FRAMES) {
            int n = ops->decode(&d, probe + got * ch,
                                (int)(PROBE_FRAMES - got));
            if (n <= 0) break;
            got += (size_t)n;
        }
        snprintf(what, sizeof what, "seek to %.1f s then decodes audio",
                 targets_s[i]);
        xpect(what, got >= PROBE_FRAMES / 2);
        if (got < PROBE_FRAMES / 2) continue;

        if (target == 0u) {
            /*
             * Frame 0 is the top of the track, which is BEFORE the reference
             * starts — ffmpeg trimmed those samples away, so there is nothing
             * there to correlate against. The stronger statement is available
             * instead: re-seeking to 0 resets the decoder, so the audio must
             * be bit-identical to opening the file and decoding from the top.
             */
            xpect("a seek to frame 0 reproduces the opening of the track "
                  "exactly",
                  got <= whole.frames &&
                  memcmp(probe, whole.pcm,
                         got * ch * sizeof(int16_t)) == 0);
            continue;
        }

        /* Where did that actually land? Search the reference around where the
         * seek claims to be. */
        long expect_ref = (long)target + base;
        long lo = expect_ref - SEARCH_SPAN;
        if (lo < 0) lo = 0;
        long hi = expect_ref + SEARCH_SPAN;
        if ((size_t)hi + got > ref.frames) hi = (long)ref.frames - (long)got;
        if (hi < lo) hi = lo;

        double best_mse = 0.0;
        long found = mp3v_best_offset(ref.pcm + (size_t)lo * ch,
                                      ref.frames - (size_t)lo,
                                      probe, got, ch, hi - lo,
                                      (got < 4096) ? got : 4096, &best_mse);
        snprintf(what, sizeof what, "seek to %.1f s is locatable in the "
                 "reference", targets_s[i]);
        xpect(what, found >= 0);
        if (found < 0) continue;

        long landed = lo + found + base;
        long err    = landed - (long)target;
        if (err < 0) err = -err;
        double psnr = mp3v_psnr(best_mse);

        printf("target %7llu  landed %7ld  error %5ld frames (%.0f ms)  "
               "PSNR %6.2f dB\n",
               (unsigned long long)target, landed, err,
               1000.0 * (double)err / (double)rate, psnr);

        snprintf(what, sizeof what,
                 "seek to %.1f s lands within a frame + the TOC's granularity",
                 targets_s[i]);
        xpect(what, err <= tol);

        snprintf(what, sizeof what,
                 "the audio after a seek to %.1f s is the right audio",
                 targets_s[i]);
        xpect(what, psnr >= 90.0);
    }

    /* A seek then a decode to the end: the remaining count has to be about
     * what is left of the track, not the whole thing again. */
    xpect("seek to 1.0 s for the tail count",
          ops->seek(&d, rate) == DECODER_OK);
    size_t tail = 0;
    for (;;) {
        int n = ops->decode(&d, probe, PROBE_FRAMES);
        if (n <= 0) break;
        tail += (size_t)n;
    }
    long want_tail = (long)whole.frames - (long)(rate + (uint64_t)base);
    long tail_err  = (long)tail - want_tail;
    if (tail_err < 0) tail_err = -tail_err;
    printf("tail after 1.0 s: %zu frames (want ~%ld)\n", tail, want_tail);
    xpect("decoding to EOS after a seek returns what is left of the track",
          tail_err <= tol);

    /* Past the end. This is not an error: the player clamps by length, and a
     * scrub to the very end should end the track exactly as playing it out
     * would — not bounce back to 0:00. */
    xpect("a seek past the end succeeds",
          ops->seek(&d, d.total_frames * 4u) == DECODER_OK);
    xpect("...and leaves the decoder at a clean end of stream",
          ops->decode(&d, probe, PROBE_FRAMES) == 0);

    /* And the decoder is still usable afterwards. */
    xpect("seeking back to 0 after the end works",
          ops->seek(&d, 0) == DECODER_OK);
    xpect("...and decodes audio again",
          ops->decode(&d, probe, PROBE_FRAMES) > 0);

    ops->close(&d);
    free(mp3);
    mp3v_free(&ref);
    mp3v_free(&whole);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <codec-vectors-dir>\n", argv[0]);
        return 2;
    }
    if (!seek_case(argv[1], "beethoven_2s_vbr_v5_44k_stereo",
                   "beethoven_2s_vbr_v5_44k_stereo", 44100u) ||
        !seek_case(argv[1], "beethoven_2s_vbr_v2_48k_stereo",
                   "beethoven_2s_vbr_v2_48k_stereo", 48000u) ||
        !seek_case(argv[1], "beethoven_2s_vbr_v2_48k_stereo_vbri",
                   "beethoven_2s_vbr_v2_48k_stereo", 48000u)) {
        printf("mp3_seek_test: a vector could not be loaded\n");
        return 1;
    }

    if (g_fails) {
        printf("mp3_seek_test: %d check%s failed\n",
               g_fails, g_fails == 1 ? "" : "s");
        return 1;
    }
    printf("ok: MP3 seek lands where it says it does\n");
    return 0;
}
