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
#include "course_sid.h"   /* 缓冲区大小取 LAB2_BUF_SIZE */

/* PLIC 接口(plic.c):轮询路径直接消费 UART 字节后,必须从 PLIC 认领并
 * 完结该外部中断,否则会残留一个待处理中断(SEIP),等回到用户态
 * 重新开中断时被误触发 —— uartintr 再读一次已空的 RBR,产生幽灵字符。
 * 认领/完结构成"中断已处理"的语义,与 traps→devintr 路径一致。 */
int  plic_claim(void);
void plic_complete(int irq);

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
#define CONS_BUF_SIZE LAB2_BUF_SIZE  /* = 64,来自 course_sid.h 的个性化参数 */
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

/* ============ UART 接收字符的统一入口 ============
 * 归一化行结束(\r -> \n;CRLF 折叠为单个 \n),再存入环形缓冲区并回显。
 * uartintr(中断路径)与 consoleread(轮询路径)共用,保证两条路径
 * 对输入的解析完全一致。 */
static int rx_last_was_cr = 0;   /* 上一字节是否为 CR,用于折叠 CRLF */

static void
cons_rx_store(char c)
{
    if (c == '\r') {
        c = '\n';
        rx_last_was_cr = 1;
    } else if (c == '\n' && rx_last_was_cr) {
        rx_last_was_cr = 0;
        return;                   /* CRLF 的第二字节,忽略 */
    } else {
        rx_last_was_cr = 0;
    }

    cons_putc_to_buf(c);          /* 存入环形缓冲区(供 consoleread 取用) */
    consputc(c);                  /* 回显:让用户看到自己输入的字符 */
}

/* uartintr:UART 接收中断处理 —— 读 RBR(同时清除 RX 中断)交给统一入口 */
void
uartintr(void)
{
    cons_rx_store((char)*uart_thr);
}

/* consoleread:从控制台读最多 n 个字节到 user_dst
 * lab2 简化:内核与用户共用页表,可直接写入用户地址(lab3 改 copyout)
 *
 * LAB2_BUF_SEMANTICS = 1(字符流):缓冲区按字符队列组织,交付规则 ——
 * 逐个交出已到达的字节,遇到 '\n'(行结束)或达到调用方限额 n 时返回:
 *   - sh 用 n=1 逐字符取(gets 语义);
 *   - bufstorm 用大 n 每次取回一整行(不会阻塞等满 n 个字节)。
 * 字符来源:优先取环形缓冲区(uartintr 已存并回显);缓冲区空时轮询
 * UART,把到达的字节经同一归一化入口 cons_rx_store 入缓冲后再取,
 * 使中断路径与轮询路径行为一致。返回实际读取的字节数 */
int
consoleread(char *user_dst, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int ch;

        /* 等待一个已归一化的字符进入缓冲区 */
        while ((ch = cons_getc_from_buf()) < 0) {
            if (*uart_lsr & LSR_RX_READY) {
                cons_rx_store((char)*uart_thr);   /* 轮询路径入缓冲 */
                int irq = plic_claim();           /* 清掉残留的 PLIC 待处理中断 */
                if (irq)
                    plic_complete(irq);
            }
        }

        /* 写入用户缓冲区(lab2 直址,lab3 改 copyout) */
        user_dst[i] = (char)ch;

        /* 行结束:把这一行交付给调用方后返回 */
        if ((char)ch == '\n') {
            i++;
            break;
        }
    }
    return i;
}
