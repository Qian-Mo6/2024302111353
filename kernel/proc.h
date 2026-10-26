/* ============ lab4 · proc 定义 ============
 * 相对 lab3 阶段四:
 *   - 新增 struct context(ra/sp/s0–s11 共 14 callee-saved 寄存器,
 *     偏移与 kernel/swtch.S 逐条对齐,禁止改错):
 *       ra@0 sp@8 s0@16 s1@24 s2@32 s3@40 s4@48 s5@56
 *       s6@64 s7@72 s8@80 s9@88 s10@96 s11@104 共 112 字节
 *   - 新增 SLEEPING 状态(sleep/wakeup 阶段二使用,阶段一先占位);
 *   - 新增 cpu_ticks / last_slice_ticks / tickets(lab4 观测与策略字段,
 *     schedstat.patch §kernel/proc.h 约定);
 *   - allocproc 初始化 context.ra = (uint64)forkret,context.sp = kstack
 *     (首进程与 fork 子进程都从 forkret 进入内核态 → usertrapret);
 *   - scheduler/sched/yield/forkret 原型在本文件声明。
 * ======================================== */
#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "riscv.h"   /* v2 起有 include guard,可安全包含(pagetable_t) */

/* riscv.h 无 C 侧 include 守卫的历史问题已修复(v2),
 * 本地 PGSIZE 守卫仍保留,供页对齐使用 */
#ifndef PGSIZE
#define PGSIZE 4096
#endif

#define NPROC 64              /* 进程表最大容量 */

/* lab4 stage2:时钟中断与调度时间片常量 */
#define TICKS_PER_SLICE 4            /* 每进程时间片长度(时钟 tick);
                                  4 保证 200 tick 观测窗有 ~50 片,
                                  彩票份额抽样波动 <±5pp(断言 ±15pp) */
#define TIMER_INTERVAL 1000000       /* stimecmp 间隔(QEMU mtime≈10MHz,1M≈0.1s) */

/* lab4:进程状态增加 SLEEPING */
enum procstate { UNUSED, USED, RUNNING, RUNNABLE, SLEEPING, ZOMBIE };

/* lab4:swtch.S 约定的 callee-saved 寄存器上下文块
 * 偏移与 swtch.S 逐条对应,误改会静默破坏寄存器。
 * 仅包含 ra/sp/s0–s11 共 14 个被调用者保存寄存器;
 * t0–t6、a0–a7 由调用者或 ecall/陷入路径保存,不在此处。 */
struct context {
    uint64 ra;
    uint64 sp;
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

struct proc {
    enum procstate state;
    int pid;
    struct proc *parent;

    struct trapframe *trapframe;  /* 内核 VA(tf_pool 页,bss 恒等映射) */
    struct context context;      /* lab4:内核态上下文,swtch.S 切走/切回 */

    pagetable_t pagetable;        /* 本进程用户页表(根页表页物理地址) */
    uint64 kstack;                /* 内核栈顶 VA(内核页表 KSTACK 高地址映射) */
    uint64 sz;                    /* 用户地址空间大小 = 堆顶 VA(字节) */
    int exit_status;              /* exit/kill 状态,wait copyout 给父进程 */
    void *chan;                   /* lab4 stage2:sleep/wakeup 等待条件 */

    /* lab4 调度统计与策略字段(schedstat.patch §kernel/proc.h) */
    int cpu_ticks;                /* 自创建以来累计 CPU tick */
    int last_slice_ticks;         /* 最近一次连续运行的时间片长度(tick) */
    int tickets;                  /* 票数,彩票/步长策略使用;默认 60 */
    int alarm_ticks;              /* lab4 stage34:pause(n) 倒计时闹钟
                                     (kernel_ticks 绝对到期值;0=无闹钟) */
};

#endif

/* 函数原型 */
struct proc *myproc(void);
struct proc *allocproc(void);
struct proc *fork(void);
int wait(uint64 status_va);
void freeproc(struct proc *p);
void exit_current(int status);

/* lab4 调度器接口(单核简化:scheduler 为 CPU 0 常驻循环) */
void scheduler(void);
void sched(void);                 /* 从当前进程切回 scheduler */
void yield(void);                 /* 当前进程主动让出 CPU */
void forkret(void);               /* 新进程内核态入口:完成初始化后 usertrapret */
int  random(void);                /* 31 位 LFSR 伪随机,彩票策略用 */
void timer_tick_smode(void);      /* S-mode 时钟 tick 路径:pause 闹钟唤醒扫描 */

/* lab4 stage2:sleep/wakeup */
void sleep(void *chan);
void wakeup(void *chan);
