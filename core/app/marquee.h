/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/marquee.h — the one scrolling-text slot: the selected overflowing
 * row or the Now Playing title. Moved verbatim out of kernel/main.c. g_mq is
 * plain extern data: run_ui's scroll tick reads its fields and every full
 * paint clears g_mq.active.
 */

#ifndef CORE_APP_MARQUEE_H
#define CORE_APP_MARQUEE_H

#include <stdint.h>

#include "../player/player.h"
#include "../ui/text.h"
#include "ui_loop.h"

/* Marquee: a single "currently-scrolling" text target (the selected overflowing
 * row, or the now-playing title). Render fns set it while drawing; the main loop
 * scrolls it in place via a tiny partial present. `text` points at our own
 * copy (g_mq.last), so callers may pass a transient buffer safely. */
/* The fingerprint buffer must hold the LONGEST target we can be handed, or the
 * comparison below sees "changed" every frame and the scroll phase resets
 * forever (the title never actually advances). Every target is a display name
 * or a tag string, both bounded by NAME_MAX; anything longer is rejected as
 * non-scrollable in mq_set rather than silently truncated into that trap. */
#define MQ_TEXT_MAX NAME_MAX

typedef struct {
    int                active, x, y, w;
    int                tw;                 /* measured text width (px)            */
    const char        *text;
    const text_font_t *font;
    uint16_t           ink, bg;
    uint32_t           t0;                 /* phase clock origin (target start)   */
    char               last[MQ_TEXT_MAX + 1]; /* content fingerprint of that target */
    int                last_x, last_y;
    int                cy0, cy1;           /* vertical clip (keeps it in its row) */
} marquee_t;

extern marquee_t g_mq;

void draw_marquee(int x, int y, int w, const char *s, int tw,
                  const text_font_t *font, uint16_t ink, uint16_t bg,
                  uint32_t t_us, int cy0, int cy1);
void mq_text(int x, int y, int w, const char *text,
             const text_font_t *font, uint16_t ink, uint16_t bg,
             int cy0, int cy1);
void marquee_tick(ui_loop_t *L);

#endif /* CORE_APP_MARQUEE_H */
