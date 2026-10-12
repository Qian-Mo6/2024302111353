/* ============ lab2 阶段三 · plic.c PLIC 中断控制器 ============
 * PLIC 负责路由外部设备中断到 CPU
 * 阶段三目标:使能 UART0 (IRQ=10) 的 S 态外部中断
 *
 * PLIC 寄存器 (memlayout.h 已定义宏):
 *   PLIC_PRIORITY + 4*N       : source N 优先级 (0=禁用, >0 越大越优先)
 *   PLIC_SENABLE(hart)       : hart 的 S 态使能位图 (bit N=1 -> 使能 source N)
 *   PLIC_SPRIORITY(hart)     : hart 的 S 态优先级阈值 (只能响应 > 阈值的 source)
 *   PLIC_SCLAIM(hart) (读)   : 拿当前待处理 IRQ 号 (0=无)
 *   PLIC_SCLAIM(hart) (写)   : 通知 PLIC 该 IRQ 处理完毕
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"

/* plicinit:初始化 PLIC
 * 1. UART0 优先级 = 1 (默认 0=禁用)
 * 2. hart 0 的 S 态使能寄存器中开放 IRQ 10 (UART0_IRQ)
 * 3. hart 0 的 S 态优先级阈值 = 0 (接受所有 >0 的优先级) */
void
plicinit(void)
{
    /* UART0 优先级 = 1 */
    *(volatile uint32 *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;

    /* hart 0 S 态使能 bit 10 (UART0_IRQ) */
    *(volatile uint32 *)PLIC_SENABLE(0) |= (1 << UART0_IRQ);

    /* hart 0 S 态优先级阈值 = 0 */
    *(volatile uint32 *)PLIC_SPRIORITY(0) = 0;
}

/* plic_claim:从 PLIC 拿当前待处理 IRQ 号
 * 返回 0 表示无中断 (不应发生在已陷入时,但防御性返回) */
int
plic_claim(void)
{
    return (int)(*(volatile uint32 *)PLIC_SCLAIM(0));
}

/* plic_complete:通知 PLIC 该 IRQ 处理完毕,可重新发起 */
void
plic_complete(int irq)
{
    *(volatile uint32 *)PLIC_SCLAIM(0) = (uint32)irq;
}
