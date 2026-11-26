/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/statusbar.h — the top chrome: the status strip (track name or
 * clock, sleep token, padlock, battery), the list-row wrappers every list
 * screen draws with, and the top banner (the Hold and On-The-Go plates).
 * Moved verbatim out of kernel/main.c. Since phase 3 also run_ui's banner
 * pass, top_banner_tick.
 */

#ifndef CORE_APP_STATUSBAR_H
#define CORE_APP_STATUSBAR_H

#include <stdint.h>

#include "ui_loop.h"

void draw_lock_glyph(int x, int y, uint16_t c);
void status_strip_render(void);
void list_row(int r, const char *text, const char *sub, const char *right,
              int chevron, int selected, int greyed, const uint16_t *chip);
void list_row_tall(int r, const char *text, const char *sub, const char *right,
                   int chevron, int selected, int greyed, const uint16_t *chip);
void list_row_titled(int r, const char *text, const char *sub,
                     const char *right, int selected, const uint16_t *chip);
int  banner_hosted(void);
int  top_banner_h(void);
void lock_banner_render(int locked);
void otg_banner_render(void);
int  top_banner_tick(ui_loop_t *L, uint32_t pump_us);

#endif /* CORE_APP_STATUSBAR_H */
