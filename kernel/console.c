/*
 * lab1 阶段三 · console.c：串口轮询驱动 + 基础控制台
 * lab2 阶段三 · 补充 UART RX 中断接收 + 回显
 * lab2 阶段四 · 补充控制台输入缓冲区 + consoleread
 *
 * 阶段四目标:实现环形输入缓冲区
 *   - uartintr() 将接收到的字符存入缓冲区并回显(中断路径)
 *   - consoleread() 从缓冲区取字符给用户程序;缓冲区空则轮询 UART
 *
 * 关键点:
 *   - 中断路径字符已在 uartintr 中回显,consoleread 从缓冲区取时不再回显
 *   - 轮询路径字符未经过 uartintr,consoleread 需自行回显
 *   - \r 统一转 \n 便于 gets() 行结束判断
 */

#include "types.h"
#include "riscv.h"

/* UART 16550 寄存器(QEMU virt 平台固定 MMIO 地址) */
#define UART0     0x10000000    /* 数据寄存器 RBR/THR */
#define UART_IER   0x10000001    /* 中断使能寄存器 IER */
#define UART_FCR   0x10000002    /* FIFO 控制寄存器 FCR */
#define UART_LCR   0x10000003    /* 线路控制寄存器 LCR */
#define UART_LSR  0x10000005    /* 线路状态寄存器 LSR */
#define LSR_THR_EMPTY  0x20     /* LSR 第 5 位:THR 空闲,可写新字符 */
#define LSR_RX_READY   0x01     /* LSR 第 0 位:RBR 有数据可读 */
#define IER_RX_EN      0x01     /* IER bit0:接收数据可用中断 */

/* 寄存器指针(volatile:防止编译器优化掉 MMIO 读写) */
static volatile uint8 *const uart_thr = (uint8 *)UART0;
static volatile uint8 *const uart_ier = (uint8 *)UART_IER;
static volatile uint8 *const uart_lsr = (uint8 *)UART_LSR;

/* ============ 阶段四:控制台输入缓冲区 ============
 * 环形缓冲区:uartintr 写入,consoleread 读出
 * lab2 无锁(单核,中断在 S 态被硬件关闭,无并发)
 * ======================================== */
#define CONS_BUF_SIZE 128
static char cons_buf[CONS_BUF_SIZE];  /* 环形缓冲区 */
static int cons_r = 0;                 /* 读指针 */
static int cons_w = 0;                 /* 写指针 */

/* cons_getc_from_buf:从缓冲区取一个字符,无数据返回 -1 */
static int
cons_getc_from_buf(void)
{
    if (cons_r == cons_w)
        return -1;  /* 空 */
    char c = cons_buf[cons_r];
    cons_r = (cons_r + 1) % CONS_BUF_SIZE;
    return c;
}

/* cons_putc_to_buf:往缓冲区存一个字符(满则丢弃) */
static void
cons_putc_to_buf(char c)
{
    int next_w = (cons_w + 1) % CONS_BUF_SIZE;
    if (next_w == cons_r)
        return;  /* 满,丢弃 */
    cons_buf[cons_w] = c;
    cons_w = next_w;
}

/* 控制台初始化接口:lab1 阶段三保留空实现
 * lab2 阶段三:调 uartinit 开 RX 中断 */
void
consoleinit(void)
{
    /* QEMU 的 UART 16550 仿真上电默认配置即可用,
     * 不需要软件初始化波特率/数据位等寄存器。
     * 真实硬件需要写 LCR(线路控制寄存器)等。
     * lab2 阶段三:开启接收数据可用中断 */
    *uart_ier |= IER_RX_EN;
}

/* 单字符同步发送:轮询 LSR 等待 THR 空闲后写入数据寄存器
 * "sync" 表示同步——阻塞等待硬件就绪,无中断驱动 */
void
uartputc_sync(char c)
{
    /* 循环读 LSR,直到第 5 位(0x20)置位
     * 第 5 位 = 1 表示 THR 已空,可写新字符
     * 不轮询直接写:可能在上一个字符还没发完时覆盖 -> 丢字符
     */
    while ((*uart_lsr & LSR_THR_EMPTY) == 0) {
        /* 忙等:QEMU 仿真下很快返回,真硬件可能等几微秒 */
    }
    *uart_thr = (uint8)c;
}

/* 字符输出上层接口:调底层 uartputc_sync
 * 分层架构:printf 阶段也只调 consputc,不直接调 uartputc_sync
 * 这样后续 console.c 加锁、加输入回显都在这一层,printf 不变 */
void
consputc(char c)
{
    uartputc_sync(c);
}

/* uartintr:UART 接收中断处理
 * 读 RBR 拿到用户输入的字符,存入缓冲区并回显
 * 读 RBR 同时会清除 UART 的 RX 中断标志位 */
void
uartintr(void)
{
    /* 读 RBR 拿字符,同时清除 UART 的 RX 中断 */
    char c = (char)*uart_thr;

    /* 简化:回车符 \r 转换为换行 \n 便于显示 */
    if (c == '\r') c = '\n';

    /* 存入环形缓冲区(供 consoleread 取用) */
    cons_putc_to_buf(c);

    /* 回显:让用户看到自己输入的字符 */
    consputc(c);
}

/* consoleread:从控制台读 n 个字节到 user_dst
 * lab2 简化:内核与用户共用页表,可直接写入用户地址(lab3 改 copyout)
 * 流程:
 *   1. 先从环形缓冲区取(中断路径已存入并回显的字符)
 *   2. 缓冲区空则轮询 UART 直接取(并回显)
 * 返回实际读取的字节数 */
int
consoleread(char *user_dst, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char c;

        /* 1. 先从缓冲区取 */
        int ch = cons_getc_from_buf();
        if (ch >= 0) {
            c = (char)ch;
            /* 中断路径已在 uartintr 中回显,此处不再回显 */
        } else {
            /* 2. 缓冲区空,轮询 UART 直接取 */
            while ((*uart_lsr & LSR_RX_READY) == 0) {
                /* 忙等:用户尚未输入 */
            }
            c = (char)*uart_thr;
            /* 轮询路径:回显(未经过 uartintr) */
            if (c == '\r') c = '\n';
            consputc(c);
        }

        /* 写入用户缓冲区(lab2 直址,lab3 改 copyout) */
        user_dst[i] = c;
    }
    return i;
}
