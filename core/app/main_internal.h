/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/main_internal.h — TRANSITIONAL. Symbols still defined in
 * kernel/main.c that an already-extracted core/app/ module needs.
 *
 * While main.c is split into modules one PR at a time (phase 2), a module
 * that moves out before one of its imports does reaches that import here:
 * the symbol loses `static` in main.c and gains a declaration below, tagged
 * with a trailing `until <module> moves` comment naming the module that
 * will own it.
 *
 * When that module moves, its declarations leave this file for the
 * module's own header. Nothing else belongs here. The rule: this file is
 * EMPTY and DELETED by the end of phase 2.
 */

#ifndef CORE_APP_MAIN_INTERNAL_H
#define CORE_APP_MAIN_INTERNAL_H

#include <stdint.h>

#include "../library/otg.h"
#include "util.h"

extern otg_list_t g_otg;                  /* until otg moves */
extern int g_menu_accum;                   /* until lists moves */
void settings_leave(void);                 /* until settings_glue moves */
void resume_capture(void);                 /* until resume moves */
int enter_standby(void);                   /* until power moves */
extern uint32_t g_present_cost_us;         /* until paint moves */

extern int      g_song_sel, g_song_accum;   /* until lists moves */

/* The On-The-Go list binds its locator pairs to songs, so it is re-bound
 * whenever the library is (library_ensure's tail). Defined with the rest of
 * the feature, below the Playlists section it shares state with. */
void otg_bind_all(void);                    /* until otg moves */
extern int8_t g_otg_flash_sign;             /* until otg moves */
extern char   g_otg_flash_label[40];        /* until otg moves */
extern char   g_otg_flash_token[16];        /* until otg moves */
extern ui_window_t g_pl_confirm;            /* until otg moves */
int otg_confirm_up(ui_window_t *w);         /* until otg moves */

#endif /* CORE_APP_MAIN_INTERNAL_H */
