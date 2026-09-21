/*
 * lab1 自测用例 · printf 边界测试程序
 *
 * 用途：lab1 说明书 §4 要求"自测用例保存在项目中备查"，本文件
 *      实现了一套针对 printf 的边界测试函数，可在内核里调
 *      run_printf_tests() 跑全套测试，对照 tests/printf_boundary.md
 *      的预期输出验证。
 *
 * 使用方法（验收时不需要跑，仅作"备查"用）：
 *   1. 在 kernel/Makefile 的 OBJS 列表末尾加：$K/printf_test.o \
 *   2. 在 kernel/main.c 的 main() 开头加：run_printf_tests();
 *   3. make clean && make && make qemu
 *   4. 对照 tests/printf_boundary.md 的预期输出验证
 *
 * 验收时 main.c 只调 printf 输出 Banner，不调本函数，所以本文件
 * 不影响 Banner 输出，仅作留档备查。
 *
 * 测试覆盖（对照 printf_boundary.md）：
 *   T01  %d 输出 0             预期: "0"
 *   T02  %d 输出 -1            预期: "-1"
 *   T03  %s 输出空字符串         预期: ""
 *   T04  %d 输出 INT64_MAX     预期: "9223372036854775807"
 *   T05  %d 输出 INT64_MIN     预期: "-9223372036854775808"（已知缺陷）
 *   T06  %x 输出 0             预期: "0x0"
 *   T07  %x 输出 96            预期: "0x60"
 *   T08  实际 Banner           预期: "2024302111353 0x2e"
 *   T09  %x 输出 1             预期: "0x1"
 *   T10  %s 输出 NULL          预期: "(null)"
 *   T11  %% 输出               预期: "100%"
 *   T12  混合占位符             预期: "name=lab1 age=1 hex=0xff"
 *   T13  未知占位符 %y          预期: "%y"
 *   T14  %x 输出 uint64 最大    预期: "0xffffffffffffffff"
 */

/* 路径相对 tests/printf_test.c 自身：types.h 和 riscv.h 都在 ../kernel/ */
#include "../kernel/types.h"
#include "../kernel/riscv.h"

/* RV64 上 long = 8 字节，能装下学号 2024302111353 和 INT64_MAX/MIN */
typedef long int64_t_;

/* 外部声明：printf.c 中定义 */
void printf(char *fmt, ...);

/* 全套边界测试入口 */
void
run_printf_tests(void)
{
    /* T01：%d 输出 0 */
    printf("[T01] ");
    printf("%d\n", (int64_t_)0);

    /* T02：%d 输出负数 */
    printf("[T02] ");
    printf("%d\n", (int64_t_)(-1));

    /* T03：%s 输出空字符串 */
    printf("[T03] ");
    printf(">");
    printf("%s", "");
    printf("<\n");

    /* T04：%d 输出 INT64_MAX（19 位数字） */
    printf("[T04] ");
    printf("%d\n", (int64_t_)9223372036854775807L);

    /* T05：%d 输出 INT64_MIN（已知缺陷：取负溢出） */
    printf("[T05] ");
    printf("%d\n", (int64_t_)(-9223372036854775807L - 1L));

    /* T06：%x 输出 0（必须 0x0，不能空） */
    printf("[T06] ");
    printf("0x%x\n", (uint64)0);

    /* T07：%x 输出 96 = 0x60 */
    printf("[T07] ");
    printf("0x%x\n", (uint64)96);

    /* T08：实际 Banner（学号 mod97 = 46 = 0x2e） */
    printf("[T08] ");
    printf("%d %x\n", (int64_t_)2024302111353L, (uint64)46);

    /* T09：%x 输出 1（单字符十六进制） */
    printf("[T09] ");
    printf("0x%x\n", (uint64)1);

    /* T10：%s 输出 NULL（必须 "(null)"） */
    printf("[T10] ");
    printf("%s\n", (char *)0);

    /* T11：%% 输出字面 % */
    printf("[T11] ");
    printf("100%%\n");

    /* T12：混合占位符 */
    printf("[T12] ");
    printf("name=%s age=%d hex=%x\n", "lab1", (int64_t_)1, (uint64)0xff);

    /* T13：未知占位符原样输出 */
    printf("[T13] ");
    printf("%y\n");

    /* T14：%x 输出 uint64 最大值（16 位十六进制） */
    printf("[T14] ");
    printf("0x%x\n", (uint64)0xFFFFFFFFFFFFFFFFUL);

    /* 测试结束分隔线 */
    printf("--- printf tests done ---\n");
}
