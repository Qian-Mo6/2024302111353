/* ============ lab2 · trap.c 陷入处理框架 ============
 * 调用链:用户态 ecall → trampoline.S(uservec) → usertrap()
 *         → usertrapret() → trampoline.S(userret) → sret → 用户态
 *
 * 阶段一:打通 U<->S 往返路径
 * 阶段二:在 syscall 分支实现 getpid/write/exit
 * 阶段三:在外部中断分支实现 devintr (UART RX 回显)
 * 阶段四:中断路径将字符存入缓冲区供 sys_read 取用
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "memlayout.h"
#include "syscall.h"

/* trampoline.S / kernelvec.S 中定义的符号 */
extern void uservec(void);
extern void userret(void);
extern void kernelvec(void);
extern char trampoline[];
void usertrapret(void);

/* 外部声明 */
extern int plic_claim(void);
extern void plic_complete(int irq);
extern void uartintr(void);

/* printf 声明(printf.c 中定义) */
void printf(char *fmt, ...);

/* devintr:外部中断处理
 * 返回 1 = 已处理,0 = 未识别中断(调用者应 panic)
 * 流程:
 *   1. 从 PLIC 拿当前 IRQ 号(读 SCLAIM)
 *   2. 按 IRQ 分发:UART0 -> uartintr (读 RBR + 回显 + 存缓冲区)
 *   3. 写 SCLAIM 通知 PLIC 处理完毕(可重新发起) */
int
devintr(void)
{
    int irq = plic_claim();
    if (irq == 0)
        return 0;

    if (irq == UART0_IRQ) {
        uartintr();
    } else {
        printf("devintr(): unhandled irq %d\n", irq);
    }

    plic_complete(irq);
    return 1;
}

/* kerneltrap:内核态(S 态)陷入处理,经 kernelvec.S 进入
 * 外部中断:直接 devintr 分发(irq=0 为伪中断,devintr 返 0,忽略);
 * 时钟中断:lab4 处理(本内核经 rdtime/stimecmp CSR,不访问 CLINT);
 * 其余为内核异常:打印现场后停机(内核 bug)。
 * 注意恢复 sepc/sstatus:处理路径若发生嵌套陷入会覆盖这两个 CSR。 */
void
kerneltrap(void)
{
    uint64 sepc    = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause  = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0) {
        printf("kerneltrap: not from S-mode scause=%x\n", scause);
        intr_off();
        for (;;) { }
    }

    if (scause == SCAUSE_TIMER_INTERRUPT) {
        /* 时钟中断:lab4 处理 */
    } else if (scause & SCAUSE_EXT_INTERRUPT) {
        devintr();
    } else {
        printf("kerneltrap: unexpected scause=%x sepc=%x stval=%x\n",
               scause, sepc, r_stval());
        intr_off();
        for (;;) { }
    }

    w_sepc(sepc);
    w_sstatus(sstatus);
}

/* usertrap:U 态陷入的 C 分发入口 */
void
usertrap(void)
{
    struct proc *p = myproc();
    uint64 scause = r_scause();

    /* stvec 切换协议(lab3 §2.2):预置 uservec 只切 satp、不切 stvec,
     * 故进入陷入处理函数的第一件事是把 stvec 切回内核向量 kernelvec;
     * 返回用户态前(usertrapret)再切回 uservec。
     * 若遗漏:内核执行期间到达的中断会再进 uservec,在内核页表下
     * 按用户语义向 TRAPFRAME 保存现场,引发无法恢复的异常风暴。 */
    w_stvec((uint64)kernelvec);

    if (scause == SCAUSE_SYSCALL) {
        p->trapframe->epc = r_sepc() + 4;
        syscall();

    } else if (scause & SCAUSE_EXT_INTERRUPT) {
        p->trapframe->epc = r_sepc();
        if (devintr() == 0) {
            printf("usertrap(): unexpected external interrupt scause=%x\n", scause);
            for (;;) { }
        }

    } else if (scause == SCAUSE_TIMER_INTERRUPT) {
        /* 时钟中断:lab4 处理 */

    } else {
        printf("usertrap(): unexpected scause %x pid=%d\n", scause, p->pid);
        printf("            sepc=%x stval=%x\n", r_sepc(), r_stval());
        for (;;) { }
    }

    usertrapret();
}

/* usertrapret:返回用户态前的环境准备 */
void
usertrapret(void)
{
    struct proc *p = myproc();
    uint64 satp = r_satp();

    w_stvec(TRAMPOLINE + ((uint64)trampoline & 0xfff));

    uint64 x = r_sstatus();
    x &= ~SSTATUS_SPP;
    x |= SSTATUS_SPIE;
    w_sstatus(x);

    w_sepc(p->trapframe->epc);

    p->trapframe->kernel_satp = satp;
    p->trapframe->kernel_sp = (uint64)p + PGSIZE;
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp();

    void (*userret_fn)(uint64) = (void *)userret;
    userret_fn(satp);
}
