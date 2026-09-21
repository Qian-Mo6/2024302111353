/*
 * lab1 阶段四 · main.c：调用 printf 输出学号 Banner
 *
 * 阶段四目标：用 printf 输出协议 0 Banner："2024302111353 0x2e\n"
 *            （学号十进制 + 空格 + 0x + mod97 小写十六进制 + 换行）
 *
 * 学号 2024302111353：
 *   - sid % 97 = 46
 *   - sid % 3  = 0  → 协议 0（明文 + 换行）
 *
 * 验收：
 *   1. make qemu 输出 "2024302111353 0x2e\n"（逐字节匹配 expect_banner.txt）
 *   2. python3 check_expect.py 2024302111353 → [ok] 协议 0
 */

#include "types.h"
#include "riscv.h"

/* 外部声明：console.c / printf.c 中定义 */
void consoleinit(void);
void printf(char *fmt, ...);

/* 学号 + 协议参数（来自 kernel/course_sid.h）
 * types.h 无 int64 typedef，用 long（RV64 上 8 字节，能装学号 2024302111353） */
#define SID      2024302111353L     /* 学号，L 后缀按 long 处理 */
#define SID_MOD97 (SID % 97)        /* sid % 97 = 46 → 0x2e */

void
main(void)
{
    /* 阶段四：初始化控制台 + printf 输出 Banner */
    consoleinit();

    /* 协议 0：sid 十进制 + 空格 + 0x{mod97 小写十六进制} + 换行
     * printf 里 %d 用 va_arg(long) 取参（学号超 int32）
     * printf 里 %x 用 va_arg(uint64) 取参，自动加 0x 前缀
     * 注意：格式串里不要再写 0x，%x 会自己加，否则输出 0x0x2e
     */
    printf("%d %x\n", (long)SID, (uint64)SID_MOD97);

    /* 死循环等待——不返回到 mret 之外 */
    while (1) {
        ;
    }
}
