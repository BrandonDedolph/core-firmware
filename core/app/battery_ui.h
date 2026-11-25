/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/battery_ui.h — the battery as the UI sees it: the cached gauge
 * sample (battery_refresh, every 5 s) and the low-battery policy's DISKSAFE /
 * SHUTOFF reactions, the charge-rate push to the charger, the status-strip
 * gauge glyph, and the charging and low-battery modals. Moved verbatim out
 * of kernel/main.c.
 */

#ifndef CORE_APP_BATTERY_UI_H
#define CORE_APP_BATTERY_UI_H

#include <stdint.h>

#include "../kernel/chargestat.h"

/* The gauge's low-battery fill colour; draw_battery_c (main.c) uses it too. */
#define BATT_LOW_RED 0xE125u        /* distinct warning red (RGB ~226,40,44)    */

extern chargestat_t g_chg;
extern int g_bat_mv;
extern int g_bat_pct;
extern int g_bat_raw;
extern int g_bat_mv_filt;

void charger_apply(void);
void charging_screen_paint(void);
void battery_modal_paint(void);
int battery_due(void);
int battery_refresh(int force);
void draw_battery(int x, int y, int pct);
int battery_glyph_key(int pct);

#endif /* CORE_APP_BATTERY_UI_H */
