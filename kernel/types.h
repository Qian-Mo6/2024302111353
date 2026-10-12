#ifndef TYPES_H
#define TYPES_H
/* ============ 预置支撑代码 · 请勿修改 ============
 * 说明: 完整类型集(用户库/内核共用的 ABI 事实)
 * 修改该支撑代码可能破坏统一接口规范；若遇到问题建议通过 git diff 核对本段代码。
 * ======================================== */
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long uint64;

typedef uint64 pde_t;

/* ============ lab2 阶段一 · trapframe 定义 ============
 * 字段顺序/偏移必须严格对应 kernel/trampoline.S 的 sd/ld 偏移:
 *   偏移 0/8/16/32 = kernel_satp/kernel_sp/kernel_trap/kernel_hartid (运行环境参数)
 *   偏移 24 = epc (C 代码用,汇编不碰)
 *   偏移 40~280 = 32 个通用寄存器 (uservec 保存,userret 恢复)
 * 偏移 112 = a0 特殊:uservec 用 sscratch 中转保存
 * ======================================== */
struct trapframe {
    uint64 kernel_satp;    // 0   - 内核页表(lab2 satp=0)
    uint64 kernel_sp;      // 8   - 内核栈顶
    uint64 kernel_trap;    // 16  - usertrap() 函数地址
    uint64 epc;            // 24  - 用户程序计数器 (C 用 w_sepc 读写,汇编不碰)
    uint64 kernel_hartid;  // 32  - 当前 CPU id
    /* --- 32 个通用寄存器:保存/恢复用户现场 --- */
    uint64 ra;             // 40
    uint64 sp;             // 48
    uint64 gp;             // 56
    uint64 tp;             // 64
    uint64 t0;             // 72
    uint64 t1;             // 80
    uint64 t2;             // 88
    uint64 s0;             // 96
    uint64 s1;             // 104
    uint64 a0;             // 112 (uservec 用 sscratch 中转保存)
    uint64 a1;             // 120
    uint64 a2;             // 128
    uint64 a3;             // 136
    uint64 a4;             // 144
    uint64 a5;             // 152
    uint64 a6;             // 160
    uint64 a7;             // 168 (syscall 号)
    uint64 s2;             // 176
    uint64 s3;             // 184
    uint64 s4;             // 192
    uint64 s5;             // 200
    uint64 s6;             // 208
    uint64 s7;             // 216
    uint64 s8;             // 224
    uint64 s9;             // 232
    uint64 s10;            // 240
    uint64 s11;            // 248
    uint64 t3;             // 256
    uint64 t4;             // 264
    uint64 t5;             // 272
    uint64 t6;             // 280
};

#endif // TYPES_H
