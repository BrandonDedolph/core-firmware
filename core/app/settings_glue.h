/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/settings_glue.h — the Settings screen's glue to the rest of the
 * firmware: the sub-screen/cursor state, pushing g_settings out to the
 * subsystems (settings_apply, sleep_timer_apply), the per-page dispatcher
 * that fills About / Boot Details / Battery (settings_render_cur), the exit
 * bookkeeping (settings_leave) and the Theme preview (theme_preview_sync).
 * Moved verbatim out of kernel/main.c, plus the screen's input handler
 * (settings_input), which drives every sub-screen and the Date & Time editor.
 */

#ifndef CORE_APP_SETTINGS_GLUE_H
#define CORE_APP_SETTINGS_GLUE_H

#include <stdint.h>

#include "hw/clickwheel.h"
#include "../fs/fat32.h"
#include "ui_loop.h"

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

/* SCR_SETTINGS's input, on every sub-screen including the Date & Time
 * editor: run_ui calls it with the drained event, its loop state, the
 * mounted volume and the event's tick while Settings is on top. */
void settings_input(const wheel_event_t *ev, ui_loop_t *L, fat32_t *fs,
                    uint32_t ev_us);

#endif /* CORE_APP_SETTINGS_GLUE_H */
