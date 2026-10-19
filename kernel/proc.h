/* ============ lab3 阶段四 · proc 定义 ============
 * 数据结构演进(lab2 -> lab3 阶段四):
 *   - 去掉内嵌 prog_page/stack_page 与全局共享页表;
 *   - 每进程独立用户页表 pagetable + 地址空间大小 sz(堆顶);
 *   - 内核栈改为内核页表 KSTACK 高地址映射(kstack 存栈顶 VA),
 *     下方留一页不映射作溢出保护(与 lab0 快照图一致);
 *   - trapframe 仍每进程独占一页(tf_pool,bss 恒等映射),
 *     其物理页在本进程用户页表的 TRAPFRAME 固定 VA 处映射(RW 无 U);
 *   - exit_status 记录退出/被杀状态,wait 时 copyout 给父进程。
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

enum procstate { UNUSED, USED, RUNNING, RUNNABLE, ZOMBIE };

struct proc {
    enum procstate state;
    int pid;
    struct proc *parent;

    struct trapframe *trapframe;  /* 内核 VA(tf_pool 页,bss 恒等映射) */

    pagetable_t pagetable;        /* 本进程用户页表(根页表页物理地址) */
    uint64 kstack;                /* 内核栈顶 VA(内核页表 KSTACK 高地址映射) */
    uint64 sz;                    /* 用户地址空间大小 = 堆顶 VA(字节) */
    int exit_status;              /* exit/kill 状态,wait copyout 给父进程 */
};

#endif

/* 函数原型 */
struct proc *myproc(void);
struct proc *allocproc(void);
struct proc *fork(void);       /* Stage6: 创建子进程,立即切换到子进程运行 */
int wait(uint64 status_va);    /* 等待 ZOMBIE 子进程并回收,可回写退出状态 */
void freeproc(struct proc *p);
void exit_current(int status); /* 终止当前进程(exit/异常被杀共用),切回父进程 */
