/* ============ lab3 阶段四 · proc.c 进程管理 ============
 * 每进程独立用户页表(lab3 阶段四页表化改造):
 *   - allocproc 建用户页表(uvmcreate)、映射 TRAPFRAME→本进程 trapframe
 *     (RW 无 U,trampoline 在 S 态经固定 VA 访问)、建 KSTACK 内核栈;
 *   - fork 逐页深拷贝父地址空间(uvmcopy,guard 空洞跳过,物理隔离);
 *   - exec 由 syscall.c 构建全新空间,全成才换表释旧;
 *   - exit/被杀(exit_current)标 ZOMBIE 后切回父进程;
 *   - wait 回收子进程,uvmfree 释放整棵页表与数据页。
 * ======================================== */
#include "types.h"
#include "proc.h"
#include "riscv.h"
#include "memlayout.h"
#include "vm.h"

/* QEMU 需要 A/D 位(riscv.h 未定义,与 vm.c 保持一致) */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

void printf(char *fmt, ...);

struct proc proc[NPROC];  /* 全局进程表 */
struct proc *curproc;     /* 当前运行进程(单核简化) */

/* trapframe 内存池:每进程独占一整页,page 对齐
 * 该物理页同时以 TRAPFRAME 固定 VA 映射进本进程用户页表 */
static char tf_pool[NPROC][PGSIZE] __attribute__((aligned(PGSIZE)));

/* myproc:返回当前 CPU 上运行的进程(单核简化) */
struct proc *
myproc(void)
{
    return curproc;
}

/* allocproc:从 proc[] 表找一个 UNUSED 槽,构建进程运行环境
 * 失败时完整回滚已分配资源,返回 0。 */
struct proc *
allocproc(void)
{
    struct proc *p;
    static int nextpid = 1;

    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED) {
            int idx = (int)(p - proc);

            /* trapframe:独占一页(bss 恒等映射,内核直接访问) */
            p->trapframe = (struct trapframe *)tf_pool[idx];
            for (int i = 0; i < (int)sizeof(*p->trapframe) / 8; i++)
                ((uint64 *)p->trapframe)[i] = 0;

            /* 内核栈:内核页表 KSTACK(idx) 高地址映射 */
            p->kstack = kstack_map(idx);
            if (p->kstack == 0) {
                p->trapframe = 0;
                return 0;
            }

            /* 用户页表:根页表 + TRAMPOLINE 同址 + TRAPFRAME 固定 VA */
            p->pagetable = uvmcreate();
            if (p->pagetable == 0) {
                kstack_unmap(idx);
                p->kstack = 0;
                p->trapframe = 0;
                return 0;
            }
            if (mappages(p->pagetable, TRAPFRAME, PGSIZE,
                         (uint64)p->trapframe,
                         PTE_R | PTE_W | PTE_A | PTE_D) != 0) {
                uvmfree(p->pagetable, 0);
                kstack_unmap(idx);
                p->pagetable = 0;
                p->kstack = 0;
                p->trapframe = 0;
                return 0;
            }

            p->pid = nextpid++;
            p->state = USED;
            p->parent = 0;
            p->sz = 0;
            p->exit_status = 0;
            return p;
        }
    }
    return 0;  /* 表满 */
}

/* fork:创建子进程
 * 地址空间:uvmcopy 逐页深拷贝(每页新物理页,物理隔离;
 * guard 空洞跳过;trapframe/trampoline 不随拷贝——前者是子进程自己的
 * 现场页,后者在 uvmcreate 时已同址映射)。
 * 简化调度:立即切换到子进程运行,子进程 exit 后切回父进程。
 * 返回:父进程得子进程指针(sys_fork 取 child->pid),子进程 a0=0 */
struct proc *
fork(void)
{
    struct proc *parent = myproc();
    struct proc *child = allocproc();
    if (!child)
        return 0;

    /* 深拷贝父地址空间 [0, sz);失败则回收子进程全部资源 */
    if (uvmcopy(parent->pagetable, child->pagetable, parent->sz) != 0) {
        freeproc(child);
        return 0;
    }
    child->sz = parent->sz;

    /* 复制 trapframe,子进程 a0=0 (fork 返回 0) */
    *child->trapframe = *parent->trapframe;
    child->trapframe->a0 = 0;

    child->parent = parent;
    curproc = child;

    return child;
}

/* wait:等待 ZOMBIE 子进程并回收
 * status_va 非 0 时把子进程退出状态 copyout 到该用户地址;
 * copyout 失败返回 -1,子进程保持 ZOMBIE(下次 wait 重试)。 */
int
wait(uint64 status_va)
{
    struct proc *p = myproc();

    for (struct proc *c = proc; c < &proc[NPROC]; c++) {
        if (c->parent == p && c->state == ZOMBIE) {
            int pid = c->pid;
            if (status_va &&
                copyout(p->pagetable, status_va,
                        (char *)&c->exit_status, sizeof(c->exit_status)) < 0)
                return -1;
            freeproc(c);
            return pid;
        }
    }
    return -1;  /* 没有 ZOMBIE 子进程 */
}

/* exit_current:终止当前进程(exit 系统调用与异常被杀共用)
 * 标 ZOMBIE 并记录状态,切回父进程;无父进程(初始进程)则停机。 */
void
exit_current(int status)
{
    struct proc *p = myproc();

    p->exit_status = status;
    p->state = ZOMBIE;

    if (p->parent) {
        curproc = p->parent;
    } else {
        /* 初始进程退出:无父进程可切,停机(后续实验改走调度器) */
        intr_off();
        for (;;) { }
    }
}

/* freeproc:释放一个进程的全部资源
 * 用户页表(含数据页、中间页表页)由 uvmfree 回收;
 * trapframe 物理页归 tf_pool(uvmfree 已跳过);内核栈映射拆除回收。 */
void
freeproc(struct proc *p)
{
    if (p->pagetable) {
        uvmfree(p->pagetable, p->sz);
        p->pagetable = 0;
    }
    if (p->kstack) {
        kstack_unmap((int)(p - proc));
        p->kstack = 0;
    }
    p->state = UNUSED;
    p->trapframe = 0;
    p->parent = 0;
    p->pid = 0;
    p->sz = 0;
    p->exit_status = 0;
}
