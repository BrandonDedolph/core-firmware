/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/search_glue.h — Music > Search's binding to the library and to
 * navigation: the one search_t instance, what ui/search.c scans and how a hit
 * reads as a row, and what a chosen hit does. Moved verbatim out of
 * kernel/main.c, plus the screen's input handler (search_input).
 */

#ifndef CORE_APP_SEARCH_GLUE_H
#define CORE_APP_SEARCH_GLUE_H

#include <stdint.h>

#include "hw/clickwheel.h"
#include "../fs/fat32.h"
#include "../ui/search.h"
#include "actions.h"
#include "ui_loop.h"

extern search_t g_search;

void       search_rescan(void);
void       search_render_cur(void);
int        search_open_hit(fat32_t *fs);
play_tap_t search_play_hit(fat32_t *fs);

/* SCR_SEARCH's input, in either mode (the ring or the results): run_ui calls
 * it with the drained event, its loop state, the mounted volume and the
 * event's tick while Search is on top. */
void search_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                  uint32_t ev_us);

#endif /* CORE_APP_SEARCH_GLUE_H */
