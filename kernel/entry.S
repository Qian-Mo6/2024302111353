/*
 * lab1 阶段一 · entry.S：汇编引导入口
 *
 * 任务：关中断 → 主核选择 → 设栈 → call start
 * 说明：此时 C 运行环境未建立，必须用汇编处理
 */

#include "course_sid.h"

/* 栈字节数：LAB1_STACK_KB 宏在 course_sid.h 中定义（4 KB）
 * 严禁硬编码 4096，必须用宏。
 * 注意：addi 立即数仅 12 位有符号（-2048~2047），4096 超界，
 * 故不用 addi 做加法，而是在 .bss 段末尾标 stack_top 符号，
 * 直接用 la 加载符号地址（la 是伪指令，支持任意地址表达式）。 */
#define STACK_BYTES (LAB1_STACK_KB * 1024)

    .section .bss                /* 未初始化数据段，启动时由 QEMU 清零 */
    .balign 16                   /* 栈按 16 字节对齐（RISC-V ABI 要求） */
stack0:
    .skip STACK_BYTES            /* 4 KB 内核栈空间 */
stack_top:                       /* 栈顶（高地址端），sp 指向这里向下生长 */

    .section .text              /* 代码段，链接脚本 ENTRY(_entry) 指向此处 */
    .globl _entry
_entry:
    csrw    mie, zero           /* 关 M 模式中断（未配 CSR 时中断进来必崩） */

    csrr    t0, mhartid         /* 读 CPU 核心编号 */
    bnez    t0, park            /* hartid != 0 是从核，跳到自旋等待 */

    la      sp, stack_top       /* sp 指向 stack0 高地址端（栈向下增长） */

    call    start               /* 进入 C 语言函数 start() */

park:
    wfi                        /* 从核低功耗等待中断（不会来，永远循环） */
    j     park
