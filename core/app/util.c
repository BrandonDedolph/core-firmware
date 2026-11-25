/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/app/util.c — see util.h.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "../library/names.h"
#include "util.h"

/*
 * Idle-task CPU sleep. Program the per-core countdown to wake this core
 * after ~`ms` milliseconds and halt until then (01-soc-pp5022.md, "Sleep
 * / wake"). We use PROC_WAIT_CNT (self-wakes on the countdown), NOT
 * PROC_SLEEP — with no interrupt controller installed yet, sleep-until-
 * interrupt would never wake. Only the CPU runs the kernel in Phase 1,
 * so CPU_CTL is correct. Three NOPs after the write per the doc's
 * pipeline rule.
 */
void cpu_wait_ms(uint8_t ms) {
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_MSEC | ms);
    __asm__ volatile("nop\n\tnop\n\tnop");
}

/* Same halt, in MICROseconds (the counter field is 8 bits, so <= 255 us). The
 * doc describes PROC_WAIT_CNT as "sleep until countdown" and does NOT promise an
 * interrupt wake, so anything used while AUDIO IS PLAYING must stay inside the
 * DMA ISR's deadline: the transfer is single-shot and re-kicked in the ISR, and
 * only the 16-frame I2S FIFO (~363 us) covers a late one. */
void cpu_wait_us(uint8_t us) {
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_USEC | us);
    __asm__ volatile("nop\n\tnop\n\tnop");
}

void ui_window_arm(ui_window_t *w)
{
    w->t0    = mmio_read32(USEC_TIMER_ADDR);
    w->armed = 1;
}

int ui_window_up(ui_window_t *w, uint32_t span, uint32_t now)
{
    if (!w->armed) return 0;
    if ((uint32_t)(now - w->t0) < span) return 1;
    w->armed = 0;                     /* expired: can't reopen on the next wrap */
    return 0;
}

/* Decimal to UART. Signed, because every battery field reads -1 on a bus
 * failure and printing that as 4294967295 would defeat the purpose. */
void uart_dec(int v)
{
    char b[12];
    if (v < 0) {
        uart_putc('-');
        v = -v;
    }
    u32_to_dec(b, (unsigned)v);
    uart_puts(b);
}
