/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/md5.h — a small MD5 (RFC 1321), header-only, for the vector
 * suites.
 *
 * Not for anything secret. It is here because two references the vectors
 * need are MD5s: a FLAC's STREAMINFO carries the MD5 libFLAC computed over
 * the unencoded audio, which is an independent truth for a lossless decode at
 * zero committed bytes; and the MP3 regression pins (*.mp3.ref.pcm.md5) are
 * `md5sum` of our own decode, which keeps a 350 KB .ref.pcm per vector out of
 * the tree. Checked against the RFC 1321 test suite in md5_selftest().
 */
#ifndef CORE_TESTS_MD5_H
#define CORE_TESTS_MD5_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;          /* bytes hashed so far */
    uint8_t  buf[64];
    size_t   fill;
} md5_ctx;

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t MD5_R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_init(md5_ctx *c)
{
    c->a = 0x67452301u;
    c->b = 0xefcdab89u;
    c->c = 0x98badcfeu;
    c->d = 0x10325476u;
    c->len  = 0;
    c->fill = 0;
}

static void md5_block(md5_ctx *c, const uint8_t *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
               ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
    }
    uint32_t a = c->a, b = c->b, cc = c->c, d = c->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int      g;
        if (i < 16)      { f = (b & cc) | (~b & d);  g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d;           g = (3 * i + 5) & 15; }
        else             { f = cc ^ (b | ~d);        g = (7 * i) & 15; }
        uint32_t t = d;
        d  = cc;
        cc = b;
        uint32_t x = a + f + MD5_K[i] + m[g];
        b  = b + ((x << MD5_R[i]) | (x >> (32 - MD5_R[i])));
        a  = t;
    }
    c->a += a;
    c->b += b;
    c->c += cc;
    c->d += d;
}

static void md5_update(md5_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->fill;
        if (take > n) take = n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        n -= take;
        if (c->fill == 64) {
            md5_block(c, c->buf);
            c->fill = 0;
        }
    }
}

static void md5_final(md5_ctx *c, uint8_t out[16])
{
    uint64_t bits = c->len * 8u;
    uint8_t  pad = 0x80;
    md5_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56) md5_update(c, &pad, 1);
    uint8_t lenle[8];
    for (int i = 0; i < 8; i++) lenle[i] = (uint8_t)(bits >> (8 * i));
    md5_update(c, lenle, 8);
    uint32_t w[4] = { c->a, c->b, c->c, c->d };
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(w[i / 4] >> (8 * (i % 4)));
}

/* Lower-case hex of a digest into a 33-byte buffer. */
static void md5_hex(const uint8_t d[16], char out[33])
{
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i]     = HEX[d[i] >> 4];
        out[2 * i + 1] = HEX[d[i] & 15];
    }
    out[32] = '\0';
}

/* RFC 1321 appendix A.5. Returns 1 when the implementation is right. */
static int md5_selftest(void)
{
    static const char *const in[] = {
        "", "a", "abc", "message digest",
        "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
    };
    static const char *const want[] = {
        "d41d8cd98f00b204e9800998ecf8427e", "0cc175b9c0f1b6a831c399e269772661",
        "900150983cd24fb0d6963f7d28e17f72", "f96b697d7cb7938d525a2f31aaf161d0",
        "57edf4a22be3c955ac49da2e2107b67a",
    };
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) {
        md5_ctx c;
        uint8_t d[16];
        char    h[33];
        md5_init(&c);
        md5_update(&c, in[i], strlen(in[i]));
        md5_final(&c, d);
        md5_hex(d, h);
        if (strcmp(h, want[i]) != 0) {
            fprintf(stderr, "md5 self-test %zu: %s, want %s\n", i, h, want[i]);
            return 0;
        }
    }
    return 1;
}

#endif /* CORE_TESTS_MD5_H */
