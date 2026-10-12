/* ============ lab2 阶段四 · 自测版 main.c ============
 * 阶段二:测试三个系统调用 write / getpid / exit
 * 阶段三:测试外部中断(用户输入字符 -> 内核回显)
 * 阶段四:测试 sys_read(从控制台读 + 回显)
 *         _user_test 打印消息后调 read 等用户输入,
 *         用户在 QEMU 敲键 -> sys_read 取字符 -> write 回显
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "memlayout.h"

void consoleinit(void);
void plicinit(void);
void printf(char *fmt, ...);
void usertrapret(void);
struct proc *myproc(void);

extern char _user_test[];

pagetable_t kvminit(uint64 trampoline_pa, uint64 trapframe_pa, uint64 user_code_pa);
void kvminithart(pagetable_t kpgtbl);

extern char trampoline[];

/* 用户测试程序:write 打印消息,read 读输入,write 回显
 * 使用 PC 相对寻址(la 在 medany 模型下生成 auipc+addi)
 * 栈缓冲区用于存放 read 读取的数据 */
__asm__(
    ".global _user_test\n"
    "_user_test:\n"
    "    # write(1, msg, 17) -> 打印 hello 消息\n"
    "    li a7, 6\n"            /* SYS_write */
    "    li a0, 1\n"            /* fd = stdout */
    "    la a1, msg\n"          /* buf */
    "    li a2, 17\n"           /* len */
    "    ecall\n"
    "    # write(1, prompt, 6) -> 打印提示符\n"
    "    li a7, 6\n"            /* SYS_write */
    "    li a0, 1\n"
    "    la a1, prompt\n"
    "    li a2, 6\n"            /* len = \"echo> \" */
    "    ecall\n"
    "    # read(0, buf, 1) -> 从控制台读 1 个字符\n"
    "    li a7, 5\n"            /* SYS_read */
    "    li a0, 0\n"            /* fd = stdin */
    "    addi a1, sp, -64\n"    /* buf = 栈上缓冲区 */
    "    li a2, 1\n"            /* n = 1 */
    "    ecall\n"
    "    # write(1, buf, 1) -> 回显读取的字符\n"
    "    li a7, 6\n"            /* SYS_write */
    "    li a0, 1\n"
    "    addi a1, sp, -64\n"    /* buf */
    "    li a2, 1\n"            /* len = 1 */
    "    ecall\n"
    "    # exit(0)\n"
    "    li a7, 2\n"            /* SYS_exit */
    "    li a0, 0\n"
    "    ecall\n"
    "    j .\n"                  /* 安全兜底 */
    "msg:\n"
    "    .asciz \"hello from user!\\n\"\n"
    "prompt:\n"
    "    .asciz \"echo> \"\n"
);

void
main(void)
{
    consoleinit();
    printf("=== lab2 stage4 self-test ===\n");

    plicinit();
    printf("plic inited (UART0 IRQ=%d enabled)\n", UART0_IRQ);

    w_sie(r_sie() | SIE_SEIE);

    struct proc *p = allocproc();
    if (!p) { printf("allocproc failed\n"); while (1) {} }
    printf("proc pid=%d\n", p->pid);

    extern struct proc *curproc;
    curproc = p;

    uint64 user_code_pa = PGROUNDDOWN((uint64)_user_test);
    uint64 trampoline_pa = PGROUNDDOWN((uint64)trampoline);
    uint64 trapframe_pa  = (uint64)p->trapframe;

    p->trapframe->epc = (uint64)_user_test - user_code_pa;
    p->trapframe->sp  = 0x80000000 + 1024 * 1024;

    printf("user_epc=%x trampoline=%x tf=%x\n",
           p->trapframe->epc, trampoline_pa, trapframe_pa);

    pagetable_t kpgtbl = kvminit(trampoline_pa, trapframe_pa, user_code_pa);
    printf("kpgtbl=%x, switching satp...\n", (uint64)kpgtbl);

    kvminithart(kpgtbl);

    w_sstatus(r_sstatus() | SSTATUS_SUM);

    printf("satp OK, going to U mode...\n");
    printf("type something then Enter to test read/write\n");

    usertrapret();

    printf("ERROR: returned from usertrapret\n");
    while (1) {}
}
