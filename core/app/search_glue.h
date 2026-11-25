/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/search_glue.h — Music > Search's binding to the library and to
 * navigation: the one search_t instance, what ui/search.c scans and how a hit
 * reads as a row, and what a chosen hit does. Moved verbatim out of
 * kernel/main.c.
 */

#ifndef CORE_APP_SEARCH_GLUE_H
#define CORE_APP_SEARCH_GLUE_H

#include "../fs/fat32.h"
#include "../ui/search.h"
#include "main_internal.h"      /* play_tap_t, until actions moves */

extern search_t g_search;

void       search_rescan(void);
void       search_render_cur(void);
int        search_open_hit(fat32_t *fs);
play_tap_t search_play_hit(fat32_t *fs);

#endif /* CORE_APP_SEARCH_GLUE_H */
