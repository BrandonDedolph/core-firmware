/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/headless/hl_screen.h — reading the headless firmware's panel.
 *
 * TEXT, FOUND THE WAY IT WAS DRAWN. hl_find_text() renders the string with the
 * firmware's own renderer (ui/text.c, the same atlas) into a scratch buffer to
 * get its coverage mask, then looks for a place on the panel where every
 * fully-covered pixel is one colour and every uncovered pixel inside the
 * string's box is another. Anti-aliased edge pixels are ignored, so the match
 * holds whatever the ink and background are (a selected row is white on
 * black, a plain one black on linen) and survives a theme change — but it is
 * exact about the glyph shapes and spacing, so "Albums" does not match
 * "Album" or "Albums 2". A clipped, ellipsised or mid-marquee string is not
 * found; assert on text the screen draws whole.
 */
#ifndef CORE_TESTS_HL_SCREEN_H
#define CORE_TESTS_HL_SCREEN_H

#include <stdint.h>

typedef struct {
    int x, y, w, h;         /* the string's ink box on the panel */
    uint16_t ink, bg;
    int font;               /* index into the face list, for diagnostics */
} hl_text_hit_t;

/* 1 and *hit filled when `s` is on `panel` in any of the firmware's faces. */
int hl_find_text(const uint16_t *panel, const char *s, hl_text_hit_t *hit);

/* Same, restricted to a band of rows [y0, y1). */
int hl_find_text_in(const uint16_t *panel, const char *s, int y0, int y1,
                    hl_text_hit_t *hit);

/* Copy / compare rectangles of a 320x240 frame. */
void hl_copy_rect(uint16_t *dst, const uint16_t *panel, int x, int y, int w, int h);
int  hl_rect_equal(const uint16_t *saved, const uint16_t *panel,
                   int x, int y, int w, int h);

/* FNV-1a over a whole frame: a golden, where a golden is stable. */
uint32_t hl_frame_hash(const uint16_t *panel);

/* Pixels of `panel` equal to `c` inside a rectangle. */
int hl_count_color(const uint16_t *panel, uint16_t c, int x, int y, int w, int h);

#endif /* CORE_TESTS_HL_SCREEN_H */
