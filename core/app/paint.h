/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/paint.h — presenting and painting: the damage-only present and its
 * pacing, the partial list repaint, the screen paint dispatch, and the
 * "core: ui" stats line. Moved verbatim out of kernel/main.c.
 *
 * The counters, g_present_cost_us and g_lp are plain extern data for now
 * (phase 2 of the split): run_ui's paint path updates the counters,
 * invalidates g_lp and marks its rows when a cover lands.
 */

#ifndef CORE_APP_PAINT_H
#define CORE_APP_PAINT_H

#include <stdint.h>

/* A list screen described generically, so one routine can repaint the rows that
 * changed without knowing which screen it is. `row` draws list row r showing
 * item `idx` (it reads that screen's own selection to decide highlighting). */
typedef struct {
    const char *title;                 /* header title (for the overlap check) */
    int         count, visible, rh, y0, sel;
    void      (*row)(int r, int idx);
    char        right[16];             /* header "n / m" value ("" = none)     */
} list_view_t;

/* What the last paint showed, so the next one can tell a plain selection move
 * from a change that needs the whole panel. `chrome` fingerprints the shared
 * furniture (status strip name, battery, lock, playing row) — if any of it
 * moved, a two-row repaint would leave the screen stale. */
typedef struct {
    int      valid, scr, depth, sel, top, count, right_w;
    uint32_t chrome, epoch;
    uint32_t rows;      /* bit r: row r's content changed under a still view
                         * (a cover chip arrived) — repaint it, nothing else */
} list_paint_t;

extern uint32_t g_present_cost_us;
extern uint32_t g_ui_render_us;
extern uint32_t g_ui_render_max_us;
extern uint32_t g_ui_present_max_us;
extern uint32_t g_ui_full, g_ui_partial;
extern uint32_t g_ui_art_reads;
extern uint32_t g_ui_art_max_us;
extern uint32_t g_ui_art_held;
extern list_paint_t g_lp;

void ui_stats_emit(void);
void ui_present_damage(void);
uint32_t present_gap_us(void);
int list_view_current(list_view_t *v);
void list_paint_note(void);
int list_repaint_partial(void);
void paint_current_screen(void);

#endif /* CORE_APP_PAINT_H */
