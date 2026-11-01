/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/mp3_vectors_test.c — the MP3s a real library holds, through
 * the device's decode path.
 *
 * mp3-freestanding runs ONE vector (the sine) through mp3_open_stream under
 * the static arena; mp3-robust builds its hostile cases by hand. This suite
 * takes the widened vectors (codec-vectors/README.md) through the same
 * streaming open, under player.c's 128 KB arena and the 48 KB MP3 budget
 * mp3-freestanding holds the decoder to:
 *
 *   - 320 kbps CBR, 48 kHz VBR with a Xing TOC, and the same 48 kHz stream
 *     with its Xing frame rewritten as a Fraunhofer VBRI frame: format,
 *     length, and a decode pinned by MD5 of our own host output
 *     (*.mp3.ref.pcm.md5 — whether it is RIGHT is mp3-accuracy's question,
 *     against ffmpeg). The VBRI file must decode to exactly the Xing file's
 *     audio, since only the tag frame differs;
 *   - the committed 128 kbps file wrapped three ways — an ID3v2.4 tag with a
 *     footer and a 330 KB APIC in front, an APEv2 + ID3v1 trailer behind,
 *     1500 bytes of sync-salted junk in front — each of which must decode to
 *     the bare file's audio sample for sample, with the same length, and
 *     whose open must SEEK over the cover art rather than read it (on the
 *     device every byte read is PIO off a spinning disk);
 *   - id3_meta_read, the library scan's parser, on the same files;
 *   - real encoder output the decoder refuses — MPEG-2.5 at 11.025 and 8 kHz
 *     (the WM8758B cannot be clocked that low) and LAME free format (frames
 *     with no length; ffmpeg refuses it too) — refused at open with a defined
 *     error, leaving the arena reusable for the next track.
 */

#include "mp3.h"
#include "id3_meta.h"
#include "arena.h"
#include "md5.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_ARENA_BYTES (128u * 1024u)    /* player.c's ARENA_BYTES      */
#define MP3_ARENA_BUDGET   (48u * 1024u)     /* mp3-freestanding's ceiling   */

static int g_fails;

static void xpect(const char *stem, const char *what, int cond)
{
    if (!cond) {
        printf("FAIL: %s: %s\n", stem, what);
        g_fails++;
    }
}

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

static uint8_t *slurp(const char *dir, const char *name, size_t *out_len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *out_len = (size_t)n;
    }
    return buf;
}

static uint8_t          g_arena_buf[PLAYER_ARENA_BYTES] __attribute__((aligned(8)));
static decoder_arena_t  g_arena;
static decoder_alloc_t  g_alloc;

typedef struct {
    int      rc;
    uint32_t rate;
    uint16_t channels;
    uint64_t total;          /* decoder_t.total_frames                  */
    int16_t *pcm;            /* malloc'd, interleaved                    */
    size_t   frames;         /* decoded                                  */
    size_t   open_read;      /* bytes READ by the open                   */
    size_t   high_water;
    int      oom;
    int      clean_eos;
} run_t;

/* Open through mp3_open_stream under the arena and decode everything in
 * deliberately odd 1000-frame chunks (never a frame multiple). */
static run_t run_stream(const uint8_t *bytes, size_t len)
{
    run_t r;
    memset(&r, 0, sizeof r);
    decoder_arena_init(&g_arena, g_arena_buf, sizeof g_arena_buf);
    g_alloc = decoder_arena_allocator(&g_arena);
    membuf_t mb = { bytes, len, 0, 0 };
    decoder_source_t src = { mem_read, mem_seek, mem_tell, &mb };
    decoder_t d;
    memset(&d, 0, sizeof d);
    r.rc = mp3_open_stream(&d, &src, &g_alloc);
    r.open_read = mb.bytes_read;
    if (r.rc == DECODER_OK) {
        r.rate = d.sample_rate;
        r.channels = d.channels;
        r.total = d.total_frames;
        size_t cap = 65536;
        r.pcm = (int16_t *)malloc(cap * 2u * sizeof(int16_t));
        int got;
        while (r.pcm && (got = d.ops->decode(&d, r.pcm + r.frames * d.channels,
                                             (int)((cap - r.frames < 1000u)
                                                   ? cap - r.frames : 1000u))) > 0) {
            r.frames += (size_t)got;
            if (cap - r.frames < 1000u) {
                cap *= 2;
                int16_t *bigger = (int16_t *)realloc(r.pcm, cap * 2u * sizeof(int16_t));
                if (!bigger) break;
                r.pcm = bigger;
            }
        }
        r.clean_eos = (d.ops->decode(&d, r.pcm, 1000) == 0);
        d.ops->close(&d);
    }
    r.high_water = g_arena.high_water;
    r.oom = g_arena.oom;
    return r;
}

static int same_pcm(const run_t *a, const run_t *b)
{
    return a->pcm && b->pcm && a->frames == b->frames &&
           a->channels == b->channels &&
           memcmp(a->pcm, b->pcm, a->frames * a->channels * sizeof(int16_t)) == 0;
}

/* The pinned MD5 (first 32 hex chars of <stem>.mp3.ref.pcm.md5). */
static int pin_matches(const char *dir, const char *stem, const run_t *r)
{
    char name[512];
    snprintf(name, sizeof name, "%s.mp3.ref.pcm.md5", stem);
    size_t   len = 0;
    uint8_t *want = slurp(dir, name, &len);
    if (!want || len < 32) {
        free(want);
        return 0;
    }
    md5_ctx c;
    uint8_t d[16];
    char    hex[33];
    md5_init(&c);
    md5_update(&c, r->pcm, r->frames * r->channels * sizeof(int16_t));
    md5_final(&c, d);
    md5_hex(d, hex);
    int ok = memcmp(hex, want, 32) == 0;
    if (!ok) printf("    %s: decode md5 %s, pinned %.32s\n", stem, hex, (char *)want);
    free(want);
    return ok;
}

static void check_run(const char *stem, const run_t *r, uint32_t rate,
                      uint16_t ch, uint64_t total)
{
    char what[160];
    xpect(stem, "mp3_open_stream under the player's arena", r->rc == DECODER_OK);
    snprintf(what, sizeof what, "reports %u Hz / %u ch / %llu frames "
             "(got %u / %u / %llu)", rate, ch, (unsigned long long)total,
             r->rate, r->channels, (unsigned long long)r->total);
    xpect(stem, what, r->rate == rate && r->channels == ch && r->total == total);
    xpect(stem, "decodes, and ends in a clean end of stream",
          r->frames > 0 && r->clean_eos);
    unsigned spf = (rate >= 32000u) ? 1152u : 576u;
    snprintf(what, sizeof what, "decodes whole frames, at least the length "
             "(%zu decoded)", r->frames);
    xpect(stem, what, r->frames % spf == 0 && r->frames >= total &&
                      r->frames <= total + 3u * spf);
    xpect(stem, "the arena never OOM'd", r->oom == 0);
    xpect(stem, "the arena stays inside the 48 KB MP3 budget",
          r->high_water < MP3_ARENA_BUDGET);
    printf("%-46s open read %6zu bytes, arena %5zu, %zu frames\n",
           stem, r->open_read, r->high_water, r->frames);
}

static void meta(const char *dir, const char *stem, flac_meta_t *m, int *rc,
                 size_t *read)
{
    char name[512];
    snprintf(name, sizeof name, "%s.mp3", stem);
    size_t   len = 0;
    uint8_t *b = slurp(dir, name, &len);
    memset(m, 0, sizeof *m);
    *rc = -2;
    *read = 0;
    if (!b) return;
    membuf_t mb = { b, len, 0, 0 };
    decoder_source_t src = { mem_read, mem_seek, mem_tell, &mb };
    *rc = id3_meta_read(&src, m);
    *read = mb.bytes_read;
    free(b);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <codec-vectors-dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    if (!md5_selftest()) {
        printf("FAIL: the MD5 implementation fails RFC 1321's test suite\n");
        return 1;
    }

    /* ---- the bare file every tag variant wraps --------------------------- */
    size_t   base_len = 0;
    uint8_t *base_b = slurp(dir, "beethoven_2s_cbr128_44k_stereo.mp3", &base_len);
    if (!base_b) {
        fprintf(stderr, "cannot read the base vector\n");
        return 2;
    }
    run_t base = run_stream(base_b, base_len);
    check_run("beethoven_2s_cbr128_44k_stereo", &base, 44100, 2, 88200);

    static const char *const variants[] = {
        "beethoven_2s_cbr128_44k_stereo_id3v24_apic",
        "beethoven_2s_cbr128_44k_stereo_ape_id3v1",
        "beethoven_2s_cbr128_44k_stereo_junk",
    };
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        char name[512];
        snprintf(name, sizeof name, "%s.mp3", variants[i]);
        size_t   len = 0;
        uint8_t *b = slurp(dir, name, &len);
        if (!b) {
            printf("FAIL: %s: cannot read the vector\n", variants[i]);
            g_fails++;
            continue;
        }
        run_t r = run_stream(b, len);
        check_run(variants[i], &r, 44100, 2, 88200);
        xpect(variants[i], "decodes to the bare file's audio, sample for sample",
              same_pcm(&r, &base));
        /* The 330 KB cover must be stepped over by its declared size. 16 KB
         * covers the tag header, the first-frame hunt and the tail probe. */
        xpect(variants[i], "the open reads no more than 16 KB (tags are "
              "seeked over, not read)", r.open_read < 16u * 1024u);
        free(r.pcm);
        free(b);
    }

    /* ---- new streams: pinned against our own decode ---------------------- */
    run_t xing = { 0 };
    static const struct {
        const char *stem;
        uint32_t    rate;
        uint64_t    total;
    } fresh[] = {
        /* 2 s x 44100, less nothing: the LAME tag's delay+padding trim it. */
        { "beethoven_2s_cbr320_44k_stereo",      44100, 88200 },
        { "beethoven_2s_vbr_v2_48k_stereo",      48000, 96000 },
        /* VBRI has no LAME delay/padding fields: frames x 1152, untrimmed —
         * which is also exactly what ffprobe reports for it (2.040 s). */
        { "beethoven_2s_vbr_v2_48k_stereo_vbri", 48000, 85u * 1152u },
    };
    for (size_t i = 0; i < sizeof fresh / sizeof fresh[0]; i++) {
        char name[512];
        snprintf(name, sizeof name, "%s.mp3", fresh[i].stem);
        size_t   len = 0;
        uint8_t *b = slurp(dir, name, &len);
        if (!b) {
            printf("FAIL: %s: cannot read the vector\n", fresh[i].stem);
            g_fails++;
            continue;
        }
        run_t r = run_stream(b, len);
        check_run(fresh[i].stem, &r, fresh[i].rate, 2, fresh[i].total);
        xpect(fresh[i].stem, "the decode matches its pinned MD5 "
              "(*.mp3.ref.pcm.md5)", r.pcm && pin_matches(dir, fresh[i].stem, &r));
        if (i == 1) {
            xing = r;                       /* kept for the VBRI comparison */
        } else {
            if (i == 2) {
                xpect(fresh[i].stem, "decodes to exactly the Xing file's audio "
                      "(only the tag frame differs)", same_pcm(&r, &xing));
                xpect(fresh[i].stem, "every decoded frame is one VBRI counted",
                      r.frames == fresh[i].total);
            }
            free(r.pcm);
        }
        free(b);
    }
    free(xing.pcm);

    /* ---- the library scan's parser on the same files ---------------------- */
    {
        flac_meta_t m;
        int    rc;
        size_t rd;
        const char *s = "beethoven_2s_cbr128_44k_stereo_id3v24_apic";
        meta(dir, s, &m, &rc, &rd);
        xpect(s, "id3_meta_read parses it", rc == 0 && m.have == 1);
        xpect(s, "v2.4 UTF-8 TIT2",
              strcmp(m.title, "Allegro con brio \xE2\x80\x94 \xC3\x9C" "bung") == 0);
        xpect(s, "TPE1", strcmp(m.artist, "Test Orchestra") == 0);
        xpect(s, "TALB keeps its accent", strcmp(m.album, "Vector Album \xC3\xA9") == 0);
        xpect(s, "TCON", strcmp(m.genre, "Classical") == 0);
        xpect(s, "TRCK 3/12 -> 3", m.track == 3);
        xpect(s, "TDRC 1804", m.year == 1804);
        xpect(s, "2 s at 44.1 kHz", m.duration_s == 2 && m.sample_rate == 44100);
        xpect(s, "TXXX ReplayGain is out of scope for MP3 (have_rg stays 0)",
              m.have_rg == 0);
        printf("%-46s id3_meta read %6zu bytes\n", s, rd);
        xpect(s, "the scan seeks over the 330 KB APIC (reads < 16 KB)",
              rd < 16u * 1024u);

        static const struct {
            const char *stem;
            uint32_t    rate;
            uint32_t    secs;
        } plain[] = {
            { "beethoven_2s_cbr128_44k_stereo_ape_id3v1", 44100, 2 },
            { "beethoven_2s_cbr128_44k_stereo_junk",      44100, 2 },
            { "beethoven_2s_cbr320_44k_stereo",           44100, 2 },
            { "beethoven_2s_vbr_v2_48k_stereo",           48000, 2 },
            { "beethoven_2s_vbr_v2_48k_stereo_vbri",      48000, 2 },
        };
        for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++) {
            meta(dir, plain[i].stem, &m, &rc, &rd);
            char what[128];
            snprintf(what, sizeof what, "id3_meta: %u s at %u Hz (got %u s, %u Hz)",
                     plain[i].secs, plain[i].rate, m.duration_s, m.sample_rate);
            xpect(plain[i].stem, what, rc == 0 && m.have == 1 &&
                  m.duration_s == plain[i].secs && m.sample_rate == plain[i].rate);
        }
    }

    /* ---- refused: real encoder output the device cannot play ------------- */
    static const struct {
        const char *stem;
        int         want_rc;
        uint32_t    meta_rate;     /* what the scan reports (0: refused too) */
    } refused[] = {
        /* The stream parses — the scan lists it with its true rate — and the
         * OPEN refuses it, so the player skips it rather than play it at the
         * wrong speed. (player.c's track_open folds every open failure into
         * PLAYER_ERR_OPEN, so today the skip is reported as a corrupt file;
         * PLAYER_ERR_RATE would be the truer answer for this one.) */
        { "beethoven_1s_mpeg25_11k_mono",               DECODER_ERR_UNSUPPORTED, 11025 },
        { "beethoven_1s_mpeg25_8k_mono",                DECODER_ERR_UNSUPPORTED, 8000 },
        /* No frame has a length, so no frame is found at all. */
        { "beethoven_quarter_s_freeformat_44k_stereo",  DECODER_ERR_INVALID,     0 },
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        const char *s = refused[i].stem;
        char name[512];
        snprintf(name, sizeof name, "%s.mp3", s);
        size_t   len = 0;
        uint8_t *b = slurp(dir, name, &len);
        if (!b) {
            printf("FAIL: %s: cannot read the vector\n", s);
            g_fails++;
            continue;
        }
        run_t r = run_stream(b, len);
        char what[128];
        snprintf(what, sizeof what, "refused at open with %d (got %d)",
                 refused[i].want_rc, r.rc);
        xpect(s, what, r.rc == refused[i].want_rc);
        xpect(s, "...without exhausting the arena", r.oom == 0);
        free(r.pcm);
        free(b);

        flac_meta_t m;
        int    rc;
        size_t rd;
        meta(dir, s, &m, &rc, &rd);
        if (refused[i].meta_rate) {
            snprintf(what, sizeof what, "id3_meta still reports the stream "
                     "(%u Hz)", refused[i].meta_rate);
            xpect(s, what, rc == 0 && m.sample_rate == refused[i].meta_rate);
        } else {
            xpect(s, "id3_meta declines it too", rc == -1 && m.have == 0);
        }
    }

    /* After the refusals, the arena the player resets per track still opens
     * a good file: a refused open must not leave it wedged. */
    run_t again = run_stream(base_b, base_len);
    xpect("arena reuse", "a good file opens and decodes identically after "
          "the refusals", again.rc == DECODER_OK && same_pcm(&again, &base));
    free(again.pcm);

    free(base.pcm);
    free(base_b);
    if (g_fails) {
        printf("mp3_vectors_test: %d check%s failed\n",
               g_fails, g_fails == 1 ? "" : "s");
        return 1;
    }
    printf("ok: every widened MP3 vector decodes (or is refused) cleanly "
           "under the player's arena\n");
    return 0;
}
