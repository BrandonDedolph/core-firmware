/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz.h — what every harness in this directory shares.
 *
 * Each harness is one LLVMFuzzerTestOneInput() over one parser that reads
 * bytes off the user's disk. They are built two ways (see meson.build):
 *
 *   - with clang -fsanitize=fuzzer,address,undefined under -Dfuzz=true, as
 *     real libFuzzer targets (`make fuzz`, docs/testing-fuzz.md);
 *   - in the ordinary host build, linked against fuzz_replay.c's main(),
 *     which runs the committed seed corpus and every regression input once.
 *     That is a `suite: 'unit'` test, so a parser change that brings an old
 *     crash back fails `meson test` (and the ASan job) without anyone having
 *     to remember to fuzz.
 *
 * "Bad" for a harness is: an ASan/UBSan report, a hang (libFuzzer -timeout,
 * or a bounded walk that turns out not to be), or a FUZZ_CHECK below — an
 * assertion about the API contract the header promises, e.g. a returned
 * length never exceeds the buffer, a string field is always terminated.
 * A FUZZ_CHECK failure abort()s, which libFuzzer reports as a crash and
 * saves the input, exactly as it does a sanitizer finding.
 */
#ifndef CORE_TESTS_FUZZ_FUZZ_H
#define CORE_TESTS_FUZZ_FUZZ_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* A contract violation: say which, then abort so the input is kept. */
#define FUZZ_CHECK(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FUZZ_CHECK failed: %s (%s:%d)\n", #cond,        \
                    __FILE__, __LINE__);                                     \
            abort();                                                         \
        }                                                                    \
    } while (0)

/*
 * KNOWN BUG, NOT FIXED HERE. A harness that has found something whose fix is
 * not obviously safe tests for the input shape with this and returns early,
 * so the fuzzer can keep looking past it. It prints once per process, so the
 * replay test's log says the gap is still open, and the regression input sits
 * in regress/<harness>/ named xfail-*. Same idea as tests/xfail.h: the bug
 * stays visible, it just stops blocking everything else. `why` must name the
 * file that owns the fix.
 *
 * Unlike tests/xfail.h, CORE_TEST_STRICT_XFAIL does not turn these into
 * failures, and there is no XPASS: the marker skips the input before the
 * assertion runs, so it cannot see the bug being fixed. Whoever fixes it
 * deletes the marker and renames the input without its xfail- prefix — the
 * input then holds the fix like any other regression.
 */
#define FUZZ_XFAIL(cond, why)                                                \
    do {                                                                     \
        if (cond) {                                                          \
            static int fuzz_xfail_said_;                                     \
            if (!fuzz_xfail_said_) {                                         \
                fuzz_xfail_said_ = 1;                                        \
                fprintf(stderr, "XFAIL (known bug, skipped): %s\n", why);    \
            }                                                                \
            return 0;                                                        \
        }                                                                    \
    } while (0)

/* NUL within `cap` bytes: the one property every fixed-size name or title
 * field must have, or the renderer walks off its end. */
static inline int fuzz_terminated(const char *s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

/*
 * Well-formed UTF-8 up to the NUL: no stray continuation, no overlong form,
 * no surrogate, nothing past U+10FFFF, no sequence cut short — and, unless
 * `allow_ctrl`, no C0 control or DEL either (the display-field policy of
 * codecs/meta_text.h and library/names.c; a FAT long name is UTF-16 off the
 * disk and may legally carry what a tag field may not). Written out
 * independently of meta_text.c on purpose — a check that calls the code
 * under test proves nothing.
 */
static inline int fuzz_utf8_ok(const char *str, int allow_ctrl)
{
    const uint8_t *s = (const uint8_t *)str;
    while (*s) {
        uint8_t c = *s;
        if (!allow_ctrl && (c < 0x20u || c == 0x7Fu)) return 0;
        if (c < 0x80u) { s++; continue; }
        uint32_t n, cp, min;
        if      ((c & 0xE0u) == 0xC0u) { n = 1; cp = c & 0x1Fu; min = 0x80u; }
        else if ((c & 0xF0u) == 0xE0u) { n = 2; cp = c & 0x0Fu; min = 0x800u; }
        else if ((c & 0xF8u) == 0xF0u) { n = 3; cp = c & 0x07u; min = 0x10000u; }
        else return 0;
        for (uint32_t i = 1; i <= n; i++) {
            if ((s[i] & 0xC0u) != 0x80u) return 0;   /* includes the NUL */
            cp = (cp << 6) | (s[i] & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
        s += n + 1u;
    }
    return 1;
}

/*
 * zlib's CRC-32 (reflected 0xEDB88320, init/final ~0), bitwise. The store
 * formats (config, evlog, otg, the index) all sign their records with it, and
 * a fuzzer flipping bytes essentially never lands a matching CRC — so a
 * harness offers a "re-sign" bit that recomputes it, letting the mutations
 * reach the decoder behind the check instead of dying at it. Written here,
 * not borrowed from any of the modules under test.
 */
static inline uint32_t fuzz_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static inline void fuzz_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- an in-memory decoder_source_t (the codec readers' byte source) ---- */

/*
 * Over the fuzzer's own buffer, so ASan sees any read past `len` as the
 * overflow it is (libFuzzer hands over an exactly-sized heap copy). `calls`
 * is a hang guard: the readers promise a bounded walk, and a source asked
 * for more than FUZZ_SRC_MAX_CALLS reads/seeks has caught one that is not.
 */
#define FUZZ_SRC_MAX_CALLS 100000u

typedef struct {
    const uint8_t *p;
    size_t         len, pos;
    uint32_t       calls;
} fuzz_mem_t;

static inline size_t fuzz_mem_read(void *ud, void *buf, size_t n)
{
    fuzz_mem_t *m = (fuzz_mem_t *)ud;
    m->calls++;
    FUZZ_CHECK(m->calls < FUZZ_SRC_MAX_CALLS);
    if (m->pos >= m->len) return 0;
    size_t left = m->len - m->pos;
    if (n > left) n = left;
    if (n) memcpy(buf, m->p + m->pos, n);
    m->pos += n;
    return n;
}

static inline int fuzz_mem_seek(void *ud, int off, int origin)
{
    fuzz_mem_t *m = (fuzz_mem_t *)ud;
    m->calls++;
    FUZZ_CHECK(m->calls < FUZZ_SRC_MAX_CALLS);
    long long base = (origin == 0) ? 0
                   : (origin == 2) ? (long long)m->len : (long long)m->pos;
    long long np = base + off;
    if (np < 0 || (unsigned long long)np > m->len) return 0;
    m->pos = (size_t)np;
    return 1;
}

static inline int64_t fuzz_mem_tell(void *ud)
{
    return (int64_t)((fuzz_mem_t *)ud)->pos;
}

#endif /* CORE_TESTS_FUZZ_FUZZ_H */
