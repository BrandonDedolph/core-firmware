/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/flac_vectors_test.c — the FLACs a real library holds.
 *
 * Every other FLAC suite decodes one vector: 44.1 kHz, 16-bit, stereo, 4096-
 * sample blocks, no tags. The surveyed library (tests/scripts/
 * survey_library.py; the distribution is in codec-vectors/README.md) is all
 * FLAC and is not that: 31 of 907 tracks are 24-bit, 898 use 4608-sample
 * blocks, 40 carry a SEEKTABLE, every one has a PICTURE of up to 622 KB after
 * a Vorbis comment of up to 128 KB, and 876 carry ReplayGain. This suite runs
 * each widened vector through what the device runs:
 *
 *   - flac_open_stream() over a pull source, with the static arena at
 *     player.c's ARENA_BYTES, asserting it never OOMs (and printing the
 *     high-water so the budget comment there stays honest);
 *   - the format and length the player sizes its clock from;
 *   - a decode checked against libFLAC's OWN truth — the MD5 its encoder put
 *     in STREAMINFO over the unencoded audio. For 16-bit sources the
 *     wrapper's output must hash to it exactly. For 24-bit sources dr_flac's
 *     native samples must hash to it (the decode is lossless), and the
 *     wrapper's 16-bit output — rounded with TPDF dither by design (see
 *     codecs/dr_flac/flac.c) — must sit within 1.5 LSB of them with no DC bias;
 *   - seeks to awkward targets (inside the short last block, the last sample,
 *     past the end), each compared against the straight decode;
 *   - flac_meta_read(), the library scan's parser: duration, rate, the
 *     SEEKTABLE count, and on the tagged vector the UTF-8 text and all four
 *     ReplayGain values — reached only by stepping over a 70 KB LYRICS comment
 *     and before a 330 KB PICTURE, which must be SKIPPED (seeked over), not
 *     read: on the device every byte read is PIO off a spinning disk.
 *
 * A 96 kHz file decodes here like any other: the codec has no rate limit.
 * The refusal is the HAL's (hal_audio_init has no 96 kHz PLL preset), and
 * player-queue asserts the player skips such a file cleanly.
 */

#include "flac.h"
#include "arena.h"
#include "flac_meta.h"
#include "dr_flac.h"
#include "md5.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* player.c's ARENA_BYTES: the arena every codec gets on the device. */
#define PLAYER_ARENA_BYTES (128u * 1024u)

static int g_fails;

static void xpect(const char *stem, const char *what, int cond)
{
    if (!cond) {
        printf("FAIL: %s: %s\n", stem, what);
        g_fails++;
    }
}

/* --- decoder_source_t over RAM, counting what is READ (not seeked) ------- */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    size_t         bytes_read;
} membuf_t;

static size_t mem_read(void *ud, void *buf, size_t n)
{
    membuf_t *m = (membuf_t *)ud;
    size_t avail = m->len - m->pos;
    if (n > avail) n = avail;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    m->bytes_read += n;
    return n;
}
static int mem_seek(void *ud, int offset, int origin)
{
    membuf_t *m = (membuf_t *)ud;
    long base = (origin == DECODER_SEEK_SET) ? 0
              : (origin == DECODER_SEEK_END) ? (long)m->len
                                             : (long)m->pos;
    long np = base + offset;
    if (np < 0 || (size_t)np > m->len) return 0;
    m->pos = (size_t)np;
    return 1;
}
static int64_t mem_tell(void *ud)
{
    return (int64_t)((membuf_t *)ud)->pos;
}

static uint8_t *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) *out_len = (size_t)n;
    return buf;
}

/* --- the vectors --------------------------------------------------------- */

enum {
    V_SINE      = 1u,   /* audio is the committed sine .pcm: memcmp it too   */
    V_TAGGED    = 2u,   /* carries the full tag set + ReplayGain + PICTURE   */
};

typedef struct {
    const char *stem;
    uint32_t    rate;
    uint16_t    channels;
    uint16_t    bps;
    uint64_t    frames;
    uint32_t    blocksize;     /* STREAMINFO max block size                 */
    uint32_t    seek_points;   /* SEEKTABLE entries flac_meta must count    */
    unsigned    flags;
} vec_t;

static const vec_t VECTORS[] = {
    { "beethoven_1s_44k_s24_stereo",            44100, 2, 24, 44100, 4096,  0, 0 },
    { "beethoven_1s_48k_s16_stereo",            48000, 2, 16, 48000, 4096,  0, 0 },
    { "beethoven_1s_96k_s24_stereo",            96000, 2, 24, 96000, 4096,  0, 0 },
    { "beethoven_1s_44k_s16_mono",              44100, 1, 16, 44100, 4096,  0, 0 },
    { "beethoven_2s_44k_s16_stereo_seektable",  44100, 2, 16, 88200, 1152, 16, 0 },
    { "beethoven_2s_44k_s16_stereo_bs4608",     44100, 2, 16, 88200, 4608,  0, 0 },
    { "sine_440hz_1s_44k_s16_stereo_picture_rg", 44100, 2, 16, 44100, 4096, 0,
      V_SINE | V_TAGGED },
    { "sine_440hz_1s_44k_s16_stereo_app_padding", 44100, 2, 16, 44100, 4096, 0,
      V_SINE },
};

/* STREAMINFO's MD5 — the first metadata block, by the spec. */
static int streaminfo_md5(const uint8_t *f, size_t len, uint8_t md5[16],
                          uint32_t *maxbs)
{
    if (len < 8 + 34 || memcmp(f, "fLaC", 4) != 0 || (f[4] & 0x7F) != 0) {
        return 0;
    }
    memcpy(md5, f + 8 + 18, 16);
    *maxbs = ((uint32_t)f[8 + 2] << 8) | f[8 + 3];
    return 1;
}

/* MD5 of interleaved samples at `bps`, as libFLAC hashes them: signed,
 * little-endian, (bps+7)/8 bytes each. */
static void md5_samples(const int32_t *s, size_t n, unsigned bps,
                        uint8_t out[16])
{
    md5_ctx c;
    md5_init(&c);
    unsigned w = (bps + 7u) / 8u;
    for (size_t i = 0; i < n; i++) {
        uint8_t b[4];
        for (unsigned k = 0; k < w; k++) b[k] = (uint8_t)((uint32_t)s[i] >> (8 * k));
        md5_update(&c, b, w);
    }
    md5_final(&c, out);
}

/* dr_flac's native-depth decode (no wrapper, libc allocator). */
static int32_t *native_decode(const uint8_t *f, size_t len, unsigned bps,
                              size_t *out_samples)
{
    drflac *fl = drflac_open_memory(f, len, NULL);
    if (!fl) return NULL;
    size_t n = (size_t)fl->totalPCMFrameCount * fl->channels;
    int32_t *s = (int32_t *)malloc(n * sizeof *s);
    drflac_uint64 got = s ? drflac_read_pcm_frames_s32(fl, fl->totalPCMFrameCount, s) : 0;
    unsigned ch = fl->channels;
    drflac_close(fl);
    if (!s) return NULL;
    /* dr_flac left-aligns to 32 bits; shift back to the source's depth. */
    for (size_t i = 0; i < (size_t)got * ch; i++) s[i] >>= (32 - (int)bps);
    *out_samples = (size_t)got * ch;
    return s;
}

/* Open through the device's path: pull source + static arena. */
static int open_stream(decoder_t *d, membuf_t *mb, decoder_source_t *src,
                       decoder_arena_t *arena, decoder_alloc_t *alloc,
                       uint8_t *arena_buf)
{
    decoder_arena_init(arena, arena_buf, PLAYER_ARENA_BYTES);
    *alloc = decoder_arena_allocator(arena);
    src->read = mem_read;
    src->seek = mem_seek;
    src->tell = mem_tell;
    src->userdata = mb;
    memset(d, 0, sizeof *d);
    return flac_open_stream(d, src, alloc);
}

/* Decode everything in deliberately odd chunks (1000 frames: never a block
 * multiple, so every block boundary lands mid-call). */
static int16_t *decode_all(decoder_t *d, uint64_t frames, size_t *got_frames)
{
    unsigned ch = d->channels;
    int16_t *out = (int16_t *)calloc((size_t)frames + 4096, ch * sizeof(int16_t));
    size_t   n = 0;
    int      got;
    while (out && (got = d->ops->decode(d, out + n * ch, 1000)) > 0) {
        n += (size_t)got;
        if (n > frames + 2048) break;          /* runaway: fail, don't overrun */
    }
    *got_frames = n;
    return out;
}

static void run_vector(const char *dir, const vec_t *v, const int16_t *sine,
                       size_t sine_len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.flac", dir, v->stem);
    size_t   len = 0;
    uint8_t *file = slurp(path, &len);
    if (!file) {
        printf("FAIL: %s: cannot read the vector\n", v->stem);
        g_fails++;
        return;
    }
    const char *s = v->stem;
    char what[160];

    uint8_t  want_md5[16];
    uint32_t maxbs = 0;
    xpect(s, "STREAMINFO is the first block and carries an MD5",
          streaminfo_md5(file, len, want_md5, &maxbs));
    snprintf(what, sizeof what, "STREAMINFO max block size is %u", v->blocksize);
    xpect(s, what, maxbs == v->blocksize);

    /* ---- the library scan's parser -------------------------------------- */
    {
        membuf_t mb = { file, len, 0, 0 };
        decoder_source_t src = { mem_read, mem_seek, mem_tell, &mb };
        flac_meta_t m;
        int rc = flac_meta_read(&src, &m);
        xpect(s, "flac_meta_read parses it", rc == 0 && m.have == 1);
        xpect(s, "flac_meta sample_rate", m.sample_rate == v->rate);
        uint32_t want_s = (uint32_t)((v->frames + v->rate / 2) / v->rate);
        snprintf(what, sizeof what, "flac_meta duration_s == %u (got %u)",
                 want_s, m.duration_s);
        xpect(s, what, m.duration_s == want_s);
        snprintf(what, sizeof what, "flac_meta seek_points == %u (got %u)",
                 v->seek_points, m.seek_points);
        xpect(s, what, m.seek_points == v->seek_points);
        printf("%-42s meta read %6zu of %7zu bytes\n", s, mb.bytes_read, len);
        /* The metadata walk must SEEK over what it does not parse — the
         * picture, padding, application data, and any comment beyond 512
         * bytes. 16 KB is generous for the comments that are read. */
        xpect(s, "flac_meta reads metadata only, seeking over the rest",
              mb.bytes_read < 16u * 1024u);

        if (v->flags & V_TAGGED) {
            xpect(s, "TITLE is the UTF-8 it was tagged with",
                  strcmp(m.title, "Allegro con brio \xE2\x80\x94 \xC3\x9C" "bung") == 0);
            xpect(s, "ARTIST", strcmp(m.artist, "Test Orchestra") == 0);
            xpect(s, "ALBUM keeps its accent",
                  strcmp(m.album, "Vector Album \xC3\xA9") == 0);
            xpect(s, "GENRE", strcmp(m.genre, "Classical") == 0);
            xpect(s, "TRACKNUMBER 3/12 -> 3", m.track == 3);
            xpect(s, "DATE 1804-05-01 -> 1804", m.year == 1804);
            xpect(s, "all four ReplayGain tags are seen past the 70 KB LYRICS",
                  m.have_rg == (FLAC_META_RG_TRACK | FLAC_META_RG_ALBUM |
                                FLAC_META_RG_TRACK_PEAK | FLAC_META_RG_ALBUM_PEAK));
            xpect(s, "REPLAYGAIN_TRACK_GAIN -6.48 dB -> -1659 q8",
                  m.rg_track_q8 == -1659);
            xpect(s, "REPLAYGAIN_ALBUM_GAIN -7.02 dB -> -1797 q8",
                  m.rg_album_q8 == -1797);
            xpect(s, "REPLAYGAIN_TRACK_PEAK 0.488281 -> ~32000 q16",
                  m.rg_track_peak_q16 >= 31999u && m.rg_track_peak_q16 <= 32001u);
            xpect(s, "REPLAYGAIN_ALBUM_PEAK 0.501 -> ~32834 q16",
                  m.rg_album_peak_q16 >= 32833u && m.rg_album_peak_q16 <= 32835u);
        }
    }

    /* ---- the device's decode path --------------------------------------- */
    static uint8_t   arena_buf[PLAYER_ARENA_BYTES] __attribute__((aligned(8)));
    decoder_arena_t  arena;
    decoder_alloc_t  alloc;
    decoder_source_t src;
    membuf_t mb = { file, len, 0, 0 };
    decoder_t d;
    int rc = open_stream(&d, &mb, &src, &arena, &alloc, arena_buf);
    xpect(s, "flac_open_stream under the player's arena", rc == DECODER_OK);
    if (rc != DECODER_OK) {
        free(file);
        return;
    }
    printf("%-42s open read %6zu bytes, arena high-water %6zu of %u\n",
           s, mb.bytes_read, arena.high_water, PLAYER_ARENA_BYTES);
    /* dr_flac learns where audio starts by walking the metadata; like the
     * scan, it must seek over the picture rather than read it. */
    xpect(s, "open reads metadata only, not the PICTURE",
          mb.bytes_read < 32u * 1024u);
    snprintf(what, sizeof what, "reports %u Hz / %u ch / %u bit / %llu frames",
             v->rate, v->channels, v->bps, (unsigned long long)v->frames);
    xpect(s, what, d.sample_rate == v->rate && d.channels == v->channels &&
                   d.bits_per_sample == v->bps && d.total_frames == v->frames);

    size_t   got = 0;
    int16_t *pcm = decode_all(&d, v->frames, &got);
    snprintf(what, sizeof what, "decodes exactly %llu frames (got %zu)",
             (unsigned long long)v->frames, got);
    xpect(s, what, pcm && got == v->frames);
    xpect(s, "decode() then reports a clean end of stream",
          d.ops->decode(&d, pcm, 1000) == 0);
    xpect(s, "the arena never OOM'd", arena.oom == 0);

    size_t   nsamp = (size_t)v->frames * v->channels;
    int32_t *native = NULL;
    if (pcm && got == v->frames) {
        uint8_t h[16];
        if (v->bps == 16) {
            int32_t *wide = (int32_t *)malloc(nsamp * sizeof *wide);
            for (size_t i = 0; i < nsamp; i++) wide[i] = pcm[i];
            md5_samples(wide, nsamp, 16, h);
            free(wide);
            xpect(s, "the decode hashes to libFLAC's STREAMINFO MD5",
                  memcmp(h, want_md5, 16) == 0);
        } else {
            size_t nn = 0;
            native = native_decode(file, len, v->bps, &nn);
            xpect(s, "dr_flac's native-depth decode is complete",
                  native && nn == nsamp);
            if (native && nn == nsamp) {
                md5_samples(native, nsamp, v->bps, h);
                xpect(s, "dr_flac's native 24-bit samples hash to libFLAC's "
                         "STREAMINFO MD5", memcmp(h, want_md5, 16) == 0);
                /* The wrapper's s16 is round(x / 256 + TPDF), TPDF within
                 * (-1, +1) LSB: |out*256 - x| < 1.5 LSB = 384. */
                int    worst = 0;
                double bias  = 0.0;
                for (size_t i = 0; i < nsamp; i++) {
                    int e = pcm[i] * 256 - native[i];
                    if (pcm[i] == 32767 || pcm[i] == -32768) continue;
                    bias += e;
                    if (e < 0) e = -e;
                    if (e > worst) worst = e;
                }
                bias /= (double)nsamp * 256.0;
                printf("%-42s s24->s16 worst %.2f LSB, mean %+.4f LSB\n",
                       s, worst / 256.0, bias);
                xpect(s, "24-bit -> 16-bit stays within 1.5 LSB (round + TPDF)",
                      worst < 384);
                xpect(s, "...with no DC bias (|mean| < 0.05 LSB: rounding, "
                         "not truncation's -0.5)", bias > -0.05 && bias < 0.05);
            }
        }
        if (v->flags & V_SINE) {
            xpect(s, "the audio is the committed sine, bit for bit",
                  sine && sine_len == nsamp * sizeof(int16_t) &&
                  memcmp(pcm, sine, sine_len) == 0);
        }
    }

    /* ---- seeks ------------------------------------------------------------ */
    if (pcm && got == v->frames) {
        uint64_t last_block = v->frames - (v->frames % v->blocksize);
        const uint64_t targets[] = {
            v->frames / 3u,
            7u,
            v->frames / 2u + 1u,
            last_block ? last_block + 5u : v->frames - 100u,  /* in the short last block */
            v->frames - 1u,                                    /* the last sample */
            0u,
        };
        static int16_t probe[2048 * 2];
        for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
            uint64_t t = targets[i];
            if (t >= v->frames) continue;
            size_t before = mb.bytes_read;
            snprintf(what, sizeof what, "seek to %llu succeeds",
                     (unsigned long long)t);
            xpect(s, what, d.ops->seek(&d, t) == DECODER_OK);
            size_t n = 0;
            int    k;
            while (n < 2048 && (k = d.ops->decode(&d, probe + n * v->channels,
                                                  (int)(2048 - n))) > 0) {
                n += (size_t)k;
            }
            size_t want_n = (v->frames - t < 2048u) ? (size_t)(v->frames - t) : 2048u;
            snprintf(what, sizeof what, "after seek to %llu: %zu frames, want %zu",
                     (unsigned long long)t, n, want_n);
            xpect(s, what, n == want_n);
            if (n != want_n) continue;
            if (v->bps == 16) {
                snprintf(what, sizeof what, "after seek to %llu the audio is "
                         "sample-exact", (unsigned long long)t);
                xpect(s, what, memcmp(probe, pcm + t * v->channels,
                                      n * v->channels * sizeof(int16_t)) == 0);
            } else if (native) {
                int worst = 0;
                for (size_t j = 0; j < n * v->channels; j++) {
                    int e = probe[j] * 256 - native[t * v->channels + j];
                    if (e < 0) e = -e;
                    if (e > worst && probe[j] != 32767 && probe[j] != -32768) worst = e;
                }
                snprintf(what, sizeof what, "after seek to %llu the audio is "
                         "the right audio (within 1.5 LSB)", (unsigned long long)t);
                xpect(s, what, worst < 384);
            }
            if (t == v->frames / 2u + 1u) {
                printf("%-42s seek to mid-file read %6zu bytes\n", s,
                       mb.bytes_read - before);
            }
        }
        xpect(s, "a seek past the end succeeds (clamped)",
              d.ops->seek(&d, v->frames * 4u) == DECODER_OK);
        xpect(s, "...and leaves a clean end of stream",
              d.ops->decode(&d, probe, 1000) == 0);
        xpect(s, "seeking back to 0 after the end decodes the opening again",
              d.ops->seek(&d, 0) == DECODER_OK &&
              d.ops->decode(&d, probe, 1000) == 1000 &&
              (v->bps != 16 ||
               memcmp(probe, pcm, 1000u * v->channels * sizeof(int16_t)) == 0));
    }

    d.ops->close(&d);
    free(native);
    free(pcm);
    free(file);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <codec-vectors-dir>\n", argv[0]);
        return 2;
    }
    if (!md5_selftest()) {
        printf("FAIL: the MD5 implementation fails RFC 1321's test suite\n");
        return 1;
    }
    char p[1024];
    snprintf(p, sizeof p, "%s/sine_440hz_1s_44k_s16_stereo.pcm", argv[1]);
    size_t   sine_len = 0;
    int16_t *sine = (int16_t *)slurp(p, &sine_len);

    for (size_t i = 0; i < sizeof VECTORS / sizeof VECTORS[0]; i++) {
        run_vector(argv[1], &VECTORS[i], sine, sine_len);
    }
    free(sine);

    if (g_fails) {
        printf("flac_vectors_test: %d check%s failed\n",
               g_fails, g_fails == 1 ? "" : "s");
        return 1;
    }
    printf("ok: every widened FLAC vector decodes losslessly under the "
           "player's arena\n");
    return 0;
}
