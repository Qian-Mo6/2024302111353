/*
 * lab1 阶段二 · start.c:M → S 特权级切换
 *
 * lab4 stage34 增量:
 *   - mtrap_c():M-mode 时钟中断处理体(由 kernelvec.S 的 mtrapvec 调用):
 *     重置 CLINT mtimecmp(偏移 0x4000!)、tick 计数、注入 SSIP 通知 S-mode。
 *   - MTIP 不委托到 S-mode(mideleg.MTIP=0),mtimecmp 只能 M-mode 写。
 *   - mtvec 指向汇编入口 mtrapvec(4 字节对齐):被中断代码可能处于
 *     trampoline(用户页表/用户栈),M-mode 访存不经 satp 翻译,
 *     必须切到专用 M-mode 栈并保存全部调用者保存寄存器。
 *
 * CLINT 偏移(xv6-riscv 原版):
 *   MTIMECMP(hart) = CLINT_BASE + 0x4000 + 8*hartid
 *   MTIME          = CLINT_BASE + 0xBFF8
 */

#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "proc.h"

#define PMP_ADDR_ALL  0x3fffffffffffffull
#define PMP_CFG_ALL_RWX 0x1f
#define TIMER_INTERVAL 1000000   /* 与 proc.h 保持一致 */

/* M-mode 时钟中断 C 处理体(mtrapvec 调用):
 *   1. 重置 mtimecmp —— MTIP 是电平触发,不重置会无限重陷;
 *   2. kernel_ticks/curproc 计数(schedstat 观测数据源);
 *   3. 注入 SSIP:S-mode 每 tick 陷入一次,负责 pause 闹钟唤醒
 *      与时间片让出(M-mode 不直接触碰 S-mode 调度逻辑)。 */
void
mtrap_c(void)
{
    *(volatile uint64 *)(CLINT_BASE + 0x4000) =
        *(volatile uint64 *)(CLINT_BASE + 0xbff8) + TIMER_INTERVAL;

    extern volatile int kernel_ticks;
    kernel_ticks++;

    extern struct proc *curproc;
    if (curproc) {
        curproc->cpu_ticks++;
        curproc->last_slice_ticks++;
    }

    asm volatile("csrs sip, %0" : : "r"(1L << 1));  /* mip.SSIP=1 */
}

void
start(void)
{
    /* ---- 1. PMP 配置:赋予 S 态访问全空间的 R/W/X 权限 ---- */
    w_pmpaddr0(PMP_ADDR_ALL);
    w_pmpcfg0(PMP_CFG_ALL_RWX);

    /* ---- 2. 异常/中断委托:MTIP(bit7) 留在 M-mode,其余委托 S-mode ---- */
    w_medeleg(0xffff);
    w_mideleg(0xffff & ~(1L << 7));

    /* ---- 3. mtvec = mtrapvec(kernelvec.S,4 字节对齐) ---- */
    extern void mtrapvec(void);
    asm volatile("csrw 0x305, %0" : : "r"(mtrapvec));

    /* ---- 4. 初始化 CLINT mtimecmp ----
     * QEMU reset 后 mtime=mtimecmp=0,MTIP 常驻 pending,
     * 必须先重置 mtimecmp 再开中断,否则开 MTIE 瞬间死循环 */
    *(volatile uint64 *)(CLINT_BASE + 0x4000) =
        *(volatile uint64 *)(CLINT_BASE + 0xbff8) + TIMER_INTERVAL;

    /* ---- 5. 开 MIE.MTIE ---- */
    w_mie(r_mie() | (1L << 7));

    /* ---- 6. 禁用分页:satp = 0 ---- */
    w_satp(0);

    /* ---- 7. mepc = main ---- */
    extern void main(void);
    w_mepc((uint64)main);

    /* ---- 8. mstatus:MPP=S(切 S 态) + MPIE=1(mret 后 MIE=1,
     *     M-mode 时钟中断开始运转) + MIE=1 ---- */
    uint64 x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;
    x |= MSTATUS_MPP_S;
    x |= (1L << 3) | (1L << 7);   /* MIE | MPIE */
    w_mstatus(x);

    /* ---- 9. mret:特权级切换 + 跳到 main ---- */
    asm volatile("mret");

    while (1) { ; }
}
