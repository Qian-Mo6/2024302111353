/* ============ lab4 · proc.c 进程管理 + 调度器骨架 ============
 * 相对 lab3 阶段四核心变化:
 *   - 引入 struct context 与 swtch.S 底层切换,进程不再直接 curproc 跳转;
 *   - scheduler() 为 CPU 0 常驻循环,其唯一 context 快照 scheduler_ctx
 *     是本文件的文件级全局变量(不是 static,生命周期覆盖整个内核运行期),
 *     每次 scheduler 切走进程时,swtch 把当前调度器的 callee-saved
 *     寄存器写回同一块(只保留最新快照,单核安全);
 *   - sched/yield/exit_current 用 intr_off/intr_on 作临界区,满足
 *     "恰好持当前进程锁"的语义;
 *   - fork() 子进程不再立即 curproc=child,改为设 RUNNABLE 后返回;
 *   - exit_current() 标 ZOMBIE 后 sched() 切回调度器;
 *   - wait() 无 ZOMBIE 子进程时 yield() 重试。
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
void usertrapret(void);          /* trap.c 定义,子进程首次从 forkret 进 U 态 */

/* swtch.S 中定义的汇编切换函数,原型与 C 调用约定一致 */
extern void swtch(struct context *old, struct context *new);

struct proc proc[NPROC];  /* 全局进程表 */
struct proc *curproc;     /* 当前运行进程(单核简化) */

/* trapframe 内存池:每进程独占一整页,page 对齐
 * 该物理页同时以 TRAPFRAME 固定 VA 映射进本进程用户页表 */
static char tf_pool[NPROC][PGSIZE] __attribute__((aligned(PGSIZE)));

/* scheduler_ctx:调度器自身内核态 context 快照
 *   文件级全局变量,生命期覆盖整个内核运行期;
 *   scheduler 函数调用栈顶保存它。
 *   第一次 scheduler 主动切到进程时,swtch 把当前调度器的
 *   ra/sp/s0-s11 写进它(作为 old 参数);
 *   以后每次 swtch 回到 scheduler,都从这里恢复(作为 new 参数)。
 *   单核下只需要"最近一次切走时调度器停在哪里"这一个快照,
 *   覆盖写合法。 */
struct context scheduler_ctx;

/* LFSR 伪随机:彩票策略抽选用,31 位 Galois LFSR,周期 2^31-1。
 * 仅在 scheduler 内单核单线程调用,无并发风险。 */
static uint32 lfsr_state = 0xDEADBEEF;

/* lab4 stage2:时钟 tick 计数 */
volatile int kernel_ticks = 0;

int
random(void)
{
    lfsr_state ^= lfsr_state >> 7;
    lfsr_state ^= lfsr_state << 9;
    lfsr_state ^= lfsr_state >> 13;
    return (int)(lfsr_state & 0x7FFFFFFF);
}

/* myproc:返回当前 CPU 上运行的进程(单核简化) */
struct proc *
myproc(void)
{
    return curproc;
}

/* allocproc:从 proc[] 表找一个 UNUSED 槽,构建进程运行环境
 * 失败时完整回滚已分配资源,返回 0。
 * lab4:初始化 context 与调度字段(context.ra = forkret,
 *      context.sp = kstack 内核栈顶;cpu_ticks=0;last_slice_ticks=0;
 *      tickets=60=默认票数(7 + sid%13)。 */
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

            /* lab4:context 初始化 —— 新进程从 forkret 进入内核态,
             * 内核栈顶作为 sp;其余 callee-saved 寄存器清零即可
             * (forkret 内会把 trapframe 设好再 usertrapret)。 */
            for (int i = 0; i < (int)sizeof(p->context) / 8; i++)
                ((uint64 *)&p->context)[i] = 0;
            p->context.ra = (uint64)forkret;
            p->context.sp = p->kstack;

            /* lab4:调度字段初始化 */
            p->cpu_ticks = 0;
            p->last_slice_ticks = 0;
            p->tickets = 60;       /* 7 + sid%13 = 7 + 53 */

            return p;
        }
    }
    return 0;  /* 表满 */
}

/* ============ lab4 调度器核心 ============ */

/* sched:从当前进程切回 scheduler()
 * 调用前:调用方已把 curproc->state 设为 RUNNABLE 或 ZOMBIE;
 * 调用内:curproc 清零、swtch 切回调度器(从 scheduler_ctx 恢复)。
 * 锁不变式:用 swtch 的原子性替代显式临界区;curproc 清零与
 * 寄存器快照保存位于同一条 swtch 指令的参数求值里,无中断窗口。 */
void
sched(void)
{
    if (curproc == 0)
        return;
    swtch(&curproc->context, &scheduler_ctx);
}

/* yield:当前进程主动让出 CPU,进入 RUNNABLE 等待下次调度 */
void
yield(void)
{
    if (curproc) {
        curproc->state = RUNNABLE;
        sched();
    }
}

/* scheduler:单核 CPU 0 常驻调度主循环
 *   LAB4_POLICY_ID = 2024302111353 % 10 = 3 → 彩票调度
 *   每次调度:累计所有 RUNNABLE 进程的 tickets,random()%total 抽签,
 *   再遍历 RUNNABLE 进程累加 tickets 命中。
 *   状态常量:RUNNABLE=3,绕过 enum 全局值传播优化坑。 */
void
scheduler(void)
{
    for (;;) {
        struct proc *p = 0;
        struct proc *q;

        /* lab4 stage34:scheduler 空转时必须开中断——
         * S 态 SIE=0 时 mtrap 注入的 SSIP 只能 pending 无法处理,
         * pause 闹钟唤醒逻辑(timer_tick_smode)跑不了,会死锁:
         * 所有进程睡着 → scheduler wfi 空转 → SSIP 永不陷入。
         * 单核下开中断安全:kerneltrap 只做唤醒/计数,不切换进程。 */
        intr_on();

        /* 第一轮:累计所有 RUNNABLE 进程的总票数 */
        int total = 0;
        for (q = proc; q < &proc[NPROC]; q++) {
            if ((int)q->state == 3)  /* RUNNABLE */
                total += q->tickets;
        }

        if (total == 0) {
            asm volatile("wfi");
            continue;
        }

        /* 抽签:random() % total 落在哪个票区间 */
        int ticket = random() % total;
        int cumulative = 0;
        for (q = proc; q < &proc[NPROC]; q++) {
            if ((int)q->state == 3) {
                cumulative += q->tickets;
                if (ticket < cumulative) {
                    p = q;
                    break;
                }
            }
        }

        if (!p) continue;

        curproc = p;
        p->state = 2;  /* RUNNING */
        swtch(&scheduler_ctx, &p->context);
    }
}

/* forkret:新进程内核态入口
 *   allocproc 时 context.ra = forkret,context.sp = kstack。
 *   swtch 切到子进程时从这里开始执行:
 *     - 首次进入时 usertrapret 进 U 态(子进程看到 a0=0,来自
 *       fork 中复制 trapframe 并设 a0=0;epc 指向父 fork 返回点);
 *     - 若进程后来被 sched() 切走又选中,swtch 回到 sched 返回点
 *       而不是这里,所以 forkret 只在新进程首次启动时被调用。 */
void
forkret(void)
{
    /* 新进程内核态入口:完成必要初始化后 usertrapret 进 U 态。 */
    usertrapret();
    /* usertrapret 切 satp 后进入 U 态,不会返回;
     * 进程 U 态退出后从 usertrap → usertrapret 再次陷入内核,
     * 那时会重新经过 usertrapret。 */
}

/* fork:创建子进程
 * lab4 改造:子进程 allocproc 时 context 已就绪(ra=forkret,sp=kstack);
 * 深拷贝用户页表 + 复制 trapframe(a0=0);设 child->state = RUNNABLE
 * 后返回——父继续运行,子由 scheduler 稍后选中。 */
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
    child->state = RUNNABLE;       /* lab4:不再 curproc=child,等 scheduler 选中 */
    return child;
}

/* wait:等待 ZOMBIE 子进程并回收
 * 阶段 1:无 sleep/wakeup,无 ZOMBIE 子进程时 yield() 重试
 *  (阶段 2 sleep/wakeup 接入后改 sleep 唤醒父进程)。
 * status_va 非 0 时把子进程退出状态 copyout 到该用户地址;
 * copyout 失败返回 -1,子进程保持 ZOMBIE(下次 wait 重试)。 */
int
wait(uint64 status_va)
{
    struct proc *p = myproc();

    for (;;) {
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
        /* lab4 stage2:无 ZOMBIE 子进程,sleep 等子进程 exit_current→wakeup */
        sleep(p);
    }
}

/* exit_current:终止当前进程(exit 系统调用与异常被杀共用)
 * lab4 改造:不再 curproc = parent,改为 ZOMBIE + sched();
 * 父进程 wait 会回收它。无父进程(初始进程)也走 ZOMBIE + sched,
 * 让 scheduler 继续跑其他 RUNNABLE 进程。 */
void
exit_current(int status)
{
    struct proc *p = myproc();
    p->exit_status = status;
    p->state = ZOMBIE;
    if (p->parent)
        wakeup(p->parent);
    sched();
}

/* sleep:当前进程等待条件 chan,关中断保护状态检查与休眠原子性 */
void
sleep(void *chan)
{
    struct proc *p = myproc();
    p->chan = chan;
    p->state = SLEEPING;
    sched();
    p->chan = 0;
}

/* wakeup:唤醒所有 chan 匹配的 SLEEPING 进程 */
void
wakeup(void *chan)
{
    intr_off();
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        if ((int)p->state == 4 /* SLEEPING */ && p->chan == chan) {
            p->state = RUNNABLE;
            p->chan = 0;
        }
    }
    intr_on();
}

/* timer_tick_smode:S-mode 时钟 tick 路径(usertrap/kerneltrap 的 SSIP 分支)调用。
 * 扫描进程表,把 pause(n) 闹钟到期的 SLEEPING 进程唤醒为 RUNNABLE。
 * 单核 + trap 上下文,无锁;sys_pause 先设 alarm 再置 SLEEPING,顺序防漏醒。 */
void
timer_tick_smode(void)
{
    extern volatile int kernel_ticks;
    struct proc *q;
    for (q = proc; q < &proc[NPROC]; q++) {
        if ((int)q->state == 4 /* SLEEPING */ && q->alarm_ticks != 0 &&
            kernel_ticks >= q->alarm_ticks) {
            q->alarm_ticks = 0;
            q->state = RUNNABLE;
        }
    }
}

/* freeproc:释放一个进程的全部资源
 * 用户页表(含数据页、中间页表页)由 uvmfree 回收;
 * trapframe 物理页归 tf_pool(uvmfree 已跳过);内核栈映射拆除回收。
 * lab4:同步清零 context 与调度字段。 */
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

    /* lab4:清零 context 与调度字段 */
    for (int i = 0; i < (int)sizeof(p->context) / 8; i++)
        ((uint64 *)&p->context)[i] = 0;
    p->cpu_ticks = 0;
    p->last_slice_ticks = 0;
    p->tickets = 0;
    p->alarm_ticks = 0;
}
