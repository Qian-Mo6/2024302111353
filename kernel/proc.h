/* ============ lab2 阶段一 · proc 定义 ============
 * 最小进程机制:够用即可(装下运行 sh、fork 子进程、wait 回收)
 * lab2 satp=0 直映,用户内存用 char mem[] 简化(记笔记:lab3 改页表)
 * ======================================== */
#ifndef PROC_H
#define PROC_H

#include "types.h"

#define NPROC 64              /* 进程表最大容量 */
#define PROC_MEM_SIZE (64*1024) /* 每进程用户内存区大小(lab2 简化) */

enum procstate { UNUSED, USED, RUNNING, RUNNABLE, ZOMBIE };

struct proc {
    enum procstate state;
    int pid;
    struct proc *parent;

    struct trapframe *trapframe;  /* 指向 trapframe 内存(由 allocproc 分配) */

    /* 用户内存:lab2 简化用 char 数组,lab3 改页表 + uvmcopy */
    char mem[PROC_MEM_SIZE];
    uint64 mem_size;
};

#endif

/* 函数原型 */
struct proc *myproc(void);
struct proc *allocproc(void);
void freeproc(struct proc *p);
