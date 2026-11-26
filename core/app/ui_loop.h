/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/ui_loop.h — the state run_ui's event loop carries from one pass
 * to the next.
 *
 * These were locals of run_ui (kernel/main.c). They are gathered here, one
 * instance on run_ui's stack reached as `L->name`, so that phase 3 of the
 * main.c split can carve the loop into phase functions taking a
 * `ui_loop_t *L` without each one growing a parameter list. Each field keeps
 * the exact name and type the local had, and run_ui still gives each one its
 * initial value at the same point the local's declaration did; the struct
 * itself is not initialised as a whole.
 *
 * Field order is not meaningful: it follows the order the locals were
 * declared in, nothing depends on it, and a field may be moved freely.
 * Per-iteration temporaries (declared inside the loop body) stay locals.
 */

#ifndef CORE_APP_UI_LOOP_H
#define CORE_APP_UI_LOOP_H

#include <stdint.h>

#include "../ui/keyhold.h"

typedef struct {
    int       dirty;
    uint32_t  np_last;
    int       np_first;
    int       np_vol_prev;             /* volume overlay was up last NP paint  */
    int       np_vol_dirty;            /* g_volume moved since the plate paint */
    uint32_t  last_present;            /* rate-limit UI presents while playing */
    uint32_t  last_bars;               /* rate-limit the now-playing bar anim  */
    uint32_t  last_chip;               /* rate-limit album-cover chip loads    */
    uint32_t  last_uistat;             /* 5 s cadence of the "core: ui" line   */
    uint32_t  last_mq;                 /* rate-limit the marquee scroll        */
    int       hold_prev;               /* seed hold-edge detect                */
    int       ext_prev;                /* seed plug-in edge detect             */
    int       banner_up;               /* a top-chrome banner is on screen     */
    char      az_prev;                 /* A-Z locator letter on screen         */
    int       toast_prev;              /* low-battery toast on screen          */
    int       bat_glyph_prev;          /* strip gauge as drawn                 */
    keyhold_t play_key;                /* PLAY: tap = pause, hold = sleep      */
    keyhold_t menu_key;                /* MENU: hold = the main menu           */
    keyhold_t row_key;                 /* SELECT on a row: tap acts, hold adds */
    int       bl_state;                /* BL_OFF / BL_DIM / BL_FULL (run_ui)   */
    int       cpu_idled;               /* core dropped to 30 MHz for deep idle */
    int       panel_slept;             /* LCD panel asleep at backlight-off    */
    uint32_t  panel_refused_at_sleep;  /* lcd_presents_refused() at the sleep  */
    uint32_t  last_input;
    int       was_active;              /* detect the active->idle edge         */
    int       last_qidx;
    uint32_t  last_oseq;               /* which track is open (seek-aim cancel) */
} ui_loop_t;

#endif /* CORE_APP_UI_LOOP_H */
