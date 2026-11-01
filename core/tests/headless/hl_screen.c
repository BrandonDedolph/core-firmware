/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/hl_screen.c — see hl_screen.h.
 */

#include "hl_screen.h"
#include "headless.h"
#include "../../ui/text.h"
#include "../../ui/palette.h"

#include <stdlib.h>
#include <string.h>

#define W HL_W
#define H HL_H

typedef const text_font_t *(*face_fn)(void);
static const face_fn k_faces[] = {
    text_font_regular_9, text_font_regular_11, text_font_regular_12,
    text_font_bold_12, text_font_bold_13, text_font_bold_18,
};
#define NFACES ((int)(sizeof k_faces / sizeof k_faces[0]))

/* One string's coverage, as offsets from its ink box's top-left. */
typedef struct {
    const char        *s;
    const text_font_t *f;
    int w, h;
    int pen_x, pen_y;       /* text_draw's pen, relative to the box          */
    int nfull, nnone;
    int *full;              /* dy * W + dx, panel-stride offsets             */
    int *none;
} mask_t;

static int build_mask(const char *s, const text_font_t *f, mask_t *m)
{
    /* Drawn twice, white on black and black on white. A pixel is UNCOVERED
     * only if neither blend moved it, and in the STRONG set only if both
     * blends gave exactly what the string's most-covered pixel gave — so every
     * strong pixel has one coverage and blends to one colour on any ground. (A
     * small regular face has no fully covered pixel at all; its strongest
     * pixels still agree with each other.) Anything else is an edge. This
     * screen only NOMINATES a place; verify_exact() decides. */
    enum { SW = 640, SH = 64 };
    static uint16_t scratch[SW * SH], inv[SW * SH];
    memset(scratch, 0, sizeof scratch);
    for (int i = 0; i < SW * SH; i++) inv[i] = 0xFFFF;
    int base = 8 + text_ascent(f);
    text_draw(scratch, SW, SH, 8, base, s, f, 0xFFFF);
    text_draw(inv, SW, SH, 8, base, s, f, 0x0000);
    uint16_t ref_s = 0, ref_i = 0xFFFF;
    for (int i = 0; i < SW * SH; i++) {
        if (scratch[i] > ref_s) { ref_s = scratch[i]; ref_i = inv[i]; }
    }
    if (ref_s == 0) return 0;
    int x0 = SW, y0 = SH, x1 = -1, y1 = -1;
    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            int i = y * SW + x;
            if (scratch[i] != 0 || inv[i] != 0xFFFF) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < 0) return 0;
    m->s = s;
    m->f = f;
    m->w = x1 - x0 + 1;
    m->h = y1 - y0 + 1;
    m->pen_x = 8 - x0;
    m->pen_y = base - y0;
    m->full = malloc(sizeof(int) * (size_t)(m->w * m->h));
    m->none = malloc(sizeof(int) * (size_t)(m->w * m->h));
    m->nfull = m->nnone = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int i = y * SW + x;
            int off = (y - y0) * W + (x - x0);
            if (scratch[i] == 0 && inv[i] == 0xFFFF) {
                m->none[m->nnone++] = off;
            } else if (scratch[i] == ref_s && inv[i] == ref_i) {
                m->full[m->nfull++] = off;
            }
        }
    }
    return m->nfull > 0 && m->nnone > 0;
}

/* The decision: some palette colour (or the observed strong colour), drawn by
 * ui/text.c over the observed ground, reproduces EVERY pixel of the box. */
static int verify_exact(const uint16_t *panel, const mask_t *m, int ox, int oy,
                        uint16_t bg, uint16_t strong, uint16_t *ink_out)
{
    static uint16_t box[W * 64];
    if (m->w > W || m->h > 64) return 0;
    for (int k = 0; k <= PAL_COUNT; k++) {
        uint16_t ink = k < PAL_COUNT ? g_pal[k] : strong;
        for (int i = 0; i < m->w * m->h; i++) box[i] = bg;
        text_draw(box, m->w, m->h, m->pen_x, m->pen_y, m->s, m->f, ink);
        int ok = 1;
        for (int r = 0; ok && r < m->h; r++) {
            ok = memcmp(box + r * m->w, panel + (oy + r) * W + ox,
                        sizeof(uint16_t) * (size_t)m->w) == 0;
        }
        if (ok) {
            *ink_out = ink;
            return 1;
        }
    }
    return 0;
}

static int match_at(const uint16_t *panel, const mask_t *m, int ox, int oy,
                    hl_text_hit_t *hit)
{
    const uint16_t *o = panel + oy * W + ox;
    uint16_t strong = o[m->full[0]], bg = o[m->none[0]];
    if (strong == bg) return 0;
    for (int i = 1; i < m->nfull; i++) {
        if (o[m->full[i]] != strong) return 0;
    }
    for (int i = 1; i < m->nnone; i++) {
        if (o[m->none[i]] != bg) return 0;
    }
    uint16_t ink;
    if (!verify_exact(panel, m, ox, oy, bg, strong, &ink)) return 0;
    hit->x = ox; hit->y = oy; hit->w = m->w; hit->h = m->h;
    hit->ink = ink; hit->bg = bg;
    return 1;
}

int hl_find_text_in(const uint16_t *panel, const char *s, int y0, int y1,
                    hl_text_hit_t *hit)
{
    hl_text_hit_t tmp;
    if (!hit) hit = &tmp;
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    for (int f = 0; f < NFACES; f++) {
        mask_t m;
        if (!build_mask(s, k_faces[f](), &m)) continue;
        int found = 0;
        for (int oy = y0; !found && oy + m.h <= y1; oy++) {
            for (int ox = 0; ox + m.w <= W; ox++) {
                if (match_at(panel, &m, ox, oy, hit)) {
                    hit->font = f;
                    found = 1;
                    break;
                }
            }
        }
        free(m.full);
        free(m.none);
        if (found) return 1;
    }
    return 0;
}

int hl_find_text(const uint16_t *panel, const char *s, hl_text_hit_t *hit)
{
    return hl_find_text_in(panel, s, 0, H, hit);
}

void hl_copy_rect(uint16_t *dst, const uint16_t *panel, int x, int y, int w, int h)
{
    for (int r = 0; r < h; r++) {
        memcpy(dst + r * w, panel + (y + r) * W + x, sizeof(uint16_t) * (size_t)w);
    }
}

int hl_rect_equal(const uint16_t *saved, const uint16_t *panel,
                  int x, int y, int w, int h)
{
    for (int r = 0; r < h; r++) {
        if (memcmp(saved + r * w, panel + (y + r) * W + x,
                   sizeof(uint16_t) * (size_t)w) != 0) {
            return 0;
        }
    }
    return 1;
}

uint32_t hl_frame_hash(const uint16_t *panel)
{
    uint32_t h = 0x811c9dc5u;
    const uint8_t *p = (const uint8_t *)panel;
    for (size_t i = 0; i < (size_t)W * H * 2; i++) {
        h = (h ^ p[i]) * 0x01000193u;
    }
    return h;
}

int hl_count_color(const uint16_t *panel, uint16_t c, int x, int y, int w, int h)
{
    int n = 0;
    for (int r = y; r < y + h; r++) {
        for (int k = x; k < x + w; k++) {
            if (panel[r * W + k] == c) n++;
        }
    }
    return n;
}
