/*
 * lab1 阶段三 · console.c：串口轮询驱动 + 基础控制台
 *
 * 阶段三目标：实现 uartputc_sync(c) 单字符同步发送（带 LSR 轮询），
 *            consputc(c) 上层接口，consoleinit() 空实现。
 *            main() 中循环输出 "Hello RISC-V OS!\n" 完整显示无乱码。
 *
 * 关键点：
 *   - 写串口前必须轮询 LSR 第 5 位（0x20），确认 THR 空闲
 *   - 不轮询直接连续写：QEMU 仿真下 UART 无 FIFO 缓冲会立即接收
 *     通常不丢字符，但真硬件或带速率限制的模拟下会丢
 *   - 分层架构：上层 consputc → 底层 uartputc_sync，printf 阶段也只调 consputc
 */

#include "types.h"
#include "riscv.h"

/* UART 16550 寄存器（QEMU virt 平台固定 MMIO 地址） */
#define UART0     0x10000000    /* 数据寄存器 RBR/THR */
#define UART_LSR  0x10000005    /* 线路状态寄存器 LSR */
#define LSR_THR_EMPTY  0x20     /* LSR 第 5 位：THR 空闲，可写新字符 */

/* 寄存器指针（volatile：防止编译器优化掉 MMIO 读写） */
static volatile uint8 *const uart_thr = (uint8 *)UART0;
static volatile uint8 *const uart_lsr = (uint8 *)UART_LSR;

/* 控制台初始化接口：lab1 阶段三保留空实现
 * （lab2 会加 console 锁、输入缓冲、consoleintr 等）
 */
void
consoleinit(void)
{
    /* QEMU 的 UART 16550 仿真上电默认配置即可用，
     * 不需要软件初始化波特率/数据位等寄存器。
     * 真实硬件需要写 LCR（线路控制寄存器）等。
     */
}

/* 单字符同步发送：轮询 LSR 等待 THR 空闲后写入数据寄存器
 * "sync" 表示同步——阻塞等待硬件就绪，无中断驱动
 */
void
uartputc_sync(char c)
{
    /* 循环读 LSR，直到第 5 位（0x20）置位
     * 第 5 位 = 1 表示 THR 已空，可写新字符
     * 不轮询直接写：可能在上一个字符还没发完时覆盖 → 丢字符
     */
    while ((*uart_lsr & LSR_THR_EMPTY) == 0) {
        /* 忙等：QEMU 仿真下很快返回，真硬件可能等几微秒 */
    }
    *uart_thr = (uint8)c;
}

/* 字符输出上层接口：调底层 uartputc_sync
 * 分层架构：printf 阶段也只调 consputc，不直接调 uartputc_sync
 * 这样后续 console.c 加锁、加输入回显都在这一层，printf 不变
 */
void
consputc(char c)
{
    uartputc_sync(c);
}
