/*
 * lab1 阶段四 · printf.c：最小格式化输出
 *
 * 阶段四目标：实现基础 printf，支持 %d %s %x %%，
 *            输出学号 Banner "2024302111353 0x2e\n"（协议 0）。
 *
 * 支持的占位符：
 *   %d   有符号十进制（int64，因学号 2024302111353 超 int32 范围）
 *   %s   字符串（char*）
 *   %x   小写十六进制（uint64，必须带 0x 前缀且无前导零，如 0x2e / 0x0）
 *   %%   字面 %
 *
 * 分层架构：printf 只调 consputc，不直接调 uartputc_sync
 *
 * 算法：
 *   - 数值转字符串用"除基取余逆序"
 *   - %x 的余数生成顺序低位→高位，需逆序输出
 *   - %d 负数：先取负、加 '-'（INT64_MIN 不处理，lab1 不会出现）
 *   - %x 0 → "0"（不是空字符串）
 *   - 不依赖任何标准库（ffreestanding）：自己实现 strlen 等
 */

#include "types.h"
#include "riscv.h"
#include <stdarg.h>   /* va_list / va_start / va_arg / va_end */

/* 外部声明：console.c 中定义 */
void consputc(char c);

/* types.h 只有 uint64，没有 int64；RV64 上 long 是 8 字节，能装下学号 */
typedef long int64_t_;

/* 输出字符串 */
static void
printstr(const char *s)
{
    if (s == (void *)0) {
        s = "(null)";
    }
    while (*s) {
        consputc(*s++);
    }
}

/* 输出有符号十进制：除基取余逆序 */
static void
printint(int64_t_ n)
{
    /* INT64_MIN 边界不处理（lab1 不会输入），普通负数取负 */
    if (n < 0) {
        consputc('-');
        n = -n;
    }

    /* 缓冲区：int64 最大 19 位 + 末尾 \0，20 字节够 */
    char buf[24];
    int i = 0;

    /* 特殊情况：n=0 直接输出 '0' */
    if (n == 0) {
        consputc('0');
        return;
    }

    /* 除基取余：余数生成顺序低位→高位，存在 buf */
    while (n > 0) {
        buf[i++] = '0' + (char)(n % 10);
        n /= 10;
    }

    /* 逆序输出 */
    while (i > 0) {
        consputc(buf[--i]);
    }
}

/* 输出小写十六进制：必须带 0x 前缀 + 无前导零
 * 例：0 → "0x0"，46 → "0x2e"，0x100 → "0x100"
 */
static void
printhex(uint64 n)
{
    /* 先输出 0x 前缀 */
    consputc('0');
    consputc('x');

    /* 缓冲区：uint64 最大 16 位十六进制 + 末尾 \0，17 字节够 */
    char buf[20];
    int i = 0;

    /* 特殊情况：n=0 输出 '0'（无前导零意味着只有一个 '0'，不是空） */
    if (n == 0) {
        consputc('0');
        return;
    }

    /* 除基取余：用 a-f 表示 10-15 */
    while (n > 0) {
        uint8 digit = (uint8)(n & 0xf);   /* n % 16 */
        if (digit < 10) {
            buf[i++] = '0' + digit;
        } else {
            buf[i++] = 'a' + (char)(digit - 10);
        }
        n >>= 4;   /* n /= 16 */
    }

    /* 逆序输出（无前导零：最高位 0 不会进 buf，自然无前导零） */
    while (i > 0) {
        consputc(buf[--i]);
    }
}

/* 最小 printf：解析 fmt 占位符并调下层 consputc 输出
 * 注意：va_arg 用 int64/uint64 取参，因为学号 2024302111353 超 int32
 */
void
printf(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            consputc(*p);
            continue;
        }

        /* 处理 % 后的转换字符 */
        p++;   /* 跳过 % */
        switch (*p) {
        case 'd':
            /* 有符号十进制：va_arg 取 long（RV64 上 long=8B，能装学号） */
            printint(va_arg(ap, int64_t_));
            break;
        case 's':
            /* 字符串：va_arg 取 char* */
            printstr(va_arg(ap, char *));
            break;
        case 'x':
            /* 小写十六进制：va_arg 取 uint64 */
            printhex(va_arg(ap, uint64));
            break;
        case '%':
            /* 字面 % */
            consputc('%');
            break;
        case '\0':
            /* 字符串末尾遇 % 结束（异常情况） */
            consputc('%');
            goto done;
        default:
            /* 未识别的占位符：原样输出 % + 字符 */
            consputc('%');
            consputc(*p);
            break;
        }
    }

done:
    va_end(ap);
}
