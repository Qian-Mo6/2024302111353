/*
 * lab1 阶段二 · start.c：M → S 特权级切换
 *
 * 阶段二目标：在 start() 中完成 M 态初始化配置，通过 mret 跳到 main()，
 *            在 main() 中向串口写 'B'，证明 CPU 成功切换至 S 态运行。
 *
 * 关键配置（执行 mret 之前必须完成）：
 *   1. PMP 配置：赋予 S 态与 U 态对全部物理地址空间的 R/W/X 权限
 *      （否则 mret 后 S 态取指即触发 cause=1 取指访问异常）
 *   2. 异常/中断委托：medeleg / mideleg 把异常和中断委托给 S 态处理
 *   3. satp = 0：禁用分页（lab1 不用虚拟内存）
 *   4. mepc = main：mret 跳转目标
 *   5. mstatus.MPP = S：mret 后进入 S 态
 *   6. mret：硬件切换特权级 + 跳到 mepc
 */

#include "types.h"
#include "riscv.h"

/* PMP 地址覆盖整个物理地址空间（RISC-V PMP 使用自然地址 = 物理地址 >> 2）。
 * 0x3fffffffffffff 自然地址 → 物理地址 0xfffffffffffffc（覆盖全空间）
 * pmpcfg0 = 0xf：第 0 个 PMP 区域配置为 TOR(Top Of Range) + R + W + X
 *   - bits[1:0] = 0b11 = TOR 模式（地址匹配 [pmpaddr[i-1], pmpaddr[i])）
 *   - bit[2] A field 不需要，bit[3-4] X/W = 1，bit[5] R = 1
 *   - 注意：在 RV64 中 pmpcfg 是 8 字节，每区域 1 字节
 *   - 实际含义见 RISC-V Privileged ISA 3.7.1
 */
#define PMP_ADDR_ALL  0x3fffffffffffffull
#define PMP_CFG_ALL_RWX 0x1f   /* L=0, A=TOR, X=1, W=1, R=1（位顺序 L|X|W|R|A|A */

void
start(void)
{
    /* ---- 1. PMP 配置：赋予 S 态访问全空间的 R/W/X 权限 ---- */
    w_pmpaddr0(PMP_ADDR_ALL);
    w_pmpcfg0(PMP_CFG_ALL_RWX);

    /* ---- 2. 异常与中断委托：把所有异常/中断委托给 S 态处理 ---- */
    /* lab1 阶段二还没 trap handler，但委托必须做，否则异常会回到 M 态（也没处理） */
    w_medeleg(0xffff);
    w_mideleg(0xffff);

    /* ---- 3. 禁用分页：satp = 0 让 S 态直接使用物理地址 ---- */
    w_satp(0);

    /* ---- 4. mepc 设为 main 函数地址：mret 后跳到这里 ---- */
    /* main 在 main.c 中定义；用 main 符号取地址（链接器解析） */
    extern void main(void);
    w_mepc((uint64)main);

    /* ---- 5. mstatus.MPP 设为 S 态：mret 后 CPU 切到 S mode ---- */
    uint64 x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;   /* 清 MPP 字段（bit 12:11） */
    x |= MSTATUS_MPP_S;       /* 设为 S（01） */
    w_mstatus(x);

    /* ---- 6. mret：硬件完成特权级切换 + 跳到 mepc ---- */
    /* 这个函数不会返回——CPU 已经跳到 main 了 */
    asm volatile("mret");

    /* 不应到达这里 */
    while (1) {
        ;
    }
}
