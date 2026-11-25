/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/settings_glue.h — the Settings screen's glue to the rest of the
 * firmware: the sub-screen/cursor state, pushing g_settings out to the
 * subsystems (settings_apply, sleep_timer_apply), the per-page dispatcher
 * that fills About / Boot Details / Battery (settings_render_cur), the exit
 * bookkeeping (settings_leave) and the Theme preview (theme_preview_sync).
 * Moved verbatim out of kernel/main.c.
 */

#ifndef CORE_APP_SETTINGS_GLUE_H
#define CORE_APP_SETTINGS_GLUE_H

extern int g_set_screen;                  /* current settings_screen_t          */
extern int g_set_sel;                     /* selection within g_set_screen      */
extern int g_set_root_sel;                /* saved ROOT selection               */
extern int g_set_accum;                   /* wheel accumulator                  */
extern int g_set_editing;                 /* editing a slider row               */

void settings_apply(void);
void sleep_timer_apply(void);
void settings_render_cur(void);
void settings_leave(void);
int  theme_preview_sync(void);

#endif /* CORE_APP_SETTINGS_GLUE_H */
