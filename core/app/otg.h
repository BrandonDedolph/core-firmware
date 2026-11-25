/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/otg.h — On-The-Go: the live list (g_otg) and its binding to the
 * library, the add/remove/play actions, the confirmation banner's state, the
 * SCR_OTG screen and Save/Delete against the slot playlists. Moved verbatim
 * out of kernel/main.c.
 *
 * NEVER include this from kernel/otg_store.c: that file has its own
 * file-local `g_otg` of a different type.
 */

#ifndef CORE_APP_OTG_H
#define CORE_APP_OTG_H

#include <stdint.h>

#include "../fs/fat32.h"
#include "../library/otg.h"
#include "util.h"               /* ui_window_t */

extern otg_list_t  g_otg;
extern int16_t     g_otg_song[OTG_MAX];
extern ui_window_t g_otg_confirm;
extern int         g_otg_sel, g_otg_accum;
extern int         g_otg_free_slot;

/* SCR_OTG's two action rows sit above the tracks, so a row index is
 * `2 + entry`. Named rather than spelled 2 everywhere: every renderer, the
 * list-view table and both Select bodies count in this space. */
enum { OTG_ROW_CLEAR = 0, OTG_ROW_SAVE = 1, OTG_ROW_FIRST = 2 };

/* The confirmation banner (otg.c has the rationale). */
#define OTG_FLASH_US 900000u

extern ui_window_t g_otg_flash;
extern int8_t      g_otg_flash_sign;
extern char        g_otg_flash_label[40];
extern char        g_otg_flash_token[16];

int  otg_confirm_up(ui_window_t *w);
void otg_touch(void);
int  otg_row_count(void);
void otg_count_token(char *dst, int n);
void otg_flash(int sign, const char *label, const char *token);

/* The On-The-Go list binds its locator pairs to songs, so it is re-bound
 * whenever the library is (library_ensure's tail). */
void otg_bind_all(void);
int  otg_add_song(int si);
int  otg_add_album(int ai);
int  otg_play(int start);
int  otg_slot_free_index(fat32_t *fs);

void otg_row_draw(int r, int idx);
void otg_header_right(char *right, int sel);
void otg_render(int sel);

int  otg_save_to_slot(fat32_t *fs);
int  otg_delete_slot(fat32_t *fs);

#endif /* CORE_APP_OTG_H */
