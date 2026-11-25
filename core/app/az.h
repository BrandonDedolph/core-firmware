/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/az.h — the A-Z locator glue (which letter a row is under, on
 * whichever list is up, and the plate that shows it), the wheel's clock and
 * the piezo navigation click. Moved verbatim out of kernel/main.c.
 *
 * kernel_main registers wheel_clock, list_initial_at, list_letter_step_idx
 * and ui_click as the ui/wheel.h seams; run_ui calls list_sel_initial,
 * az_overlay_render and ui_click directly.
 */

#ifndef CORE_APP_AZ_H
#define CORE_APP_AZ_H

#include <stdint.h>

char     list_initial_at(int idx);
char     list_sel_initial(void);
int      list_letter_step_idx(int sel, int count, int dir);
uint32_t wheel_clock(void);
void     az_overlay_render(char ch);
void     ui_click(void);

#endif /* CORE_APP_AZ_H */
