/* ============ lab3 阶段四 · 用户进程独立页表版 main.c ============
 * 启动流程:
 *   1. consoleinit/banner/kinit(物理页分配器)/plicinit/开外部中断
 *   2. kvminit + kvminithart:内核页表(恒等映射 + 外设 + TRAMPOLINE)
 *   3. allocproc:首进程用户页表 + TRAPFRAME 同址 + KSTACK 内核栈
 *   4. initcode(_user_test)装配进用户页表:RXU 代码页 + 保护页 + RWU 栈页
 *   5. usertrapret:a0 = 用户页表 satp,经 trampoline 切入 U 态
 * 用户空间指针一律经 copyin/copyout;不再使用 SSTATUS_SUM。
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "proc.h"
#include "memlayout.h"
#include "course_sid.h"   /* 学号与个性化参数(COURSE_SID / LAB2_BUF_SIZE 等) */
#include "vm.h"           /* lab3:kinit/kalloc/kfree 接口 */

/* lab3 阶段一临时验证开关:1=上电自检物理页分配器。
 * 已验证 PASS(对齐/升序/清零/守恒/触顶 32107 次后返回 0),正式构建置 0。 */
#define LAB3_KALLOC_SELFTEST 0

/* lab3 阶段二临时验证开关:1=上电自检 walk/mappages 回滚/unmappings。
 * 已验证 PASS(map+walk / 回滚守恒 / unmap+prune),正式构建置 0。 */
#define LAB3_VM_SELFTEST 0

/* lab3 阶段三临时验证开关:1=上电自检内核页表(恒等映射/外设/跳板)与 stvec。
 * 已验证 PASS(恒等映射精确至 PHYSTOP / 外设与跳板权限 / stvec=kernelvec /
 * satp 后恒等映射可读写),正式构建置 0。 */
#define LAB3_KVM_SELFTEST 0

/* lab3 阶段五临时验证开关:1=上电自检受控拷贝(copyin/copyout/copyinstr)。
 * 覆盖跨页拼接、代码页写拒绝/读允许、guard 空洞、未映射地址、
 * MAXVA 上界与溢出、零长度、copyinstr 跨页与无 NUL 超长。
 * 已验证 PASS(5/5),正式构建置 0。 */
#define LAB3_COPY_SELFTEST 0

/* lab3 阶段六临时验证开关:1=上电自检 sbrk 的 uvmalloc/uvmdealloc。
 * 覆盖 EAGER 增长(RWU 映射 + 可写)、收缩回收、TRAPFRAME 上界拒绝、
 * 配额不足时分配中途失败的回滚守恒。
 * 已验证 PASS(4/4),正式构建置 0。 */
#define LAB3_SBRK_SELFTEST 0

void consoleinit(void);
void plicinit(void);
void printf(char *fmt, ...);
void usertrapret(void);
struct proc *myproc(void);
extern void kernelvec(void);

/* QEMU 需要 A/D 位(riscv.h 未定义,与 vm.c 保持一致) */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

extern char _user_test[];

void kvminithart(pagetable_t kpgtbl);

extern char trampoline[];

/* 初始用户程序:exec("sh", 0) 启动最小 shell
 * sh 之后靠 fork/exec/wait/read/write 循环执行用户命令
 * 使用 PC 相对寻址(la 在 medany 模型下生成 auipc+addi)
 * 不需要栈 — exec 之前不调用任何需要栈的代码 */
__asm__(
    ".global _user_test\n"
    "_user_test:\n"
    "    # exec(\"sh\", 0) 加载并运行 sh\n"
    "    li a7, 7\n"            /* SYS_exec(与 kernel/syscall.h 一致) */
    "    la a0, name\n"         /* name 指向 "sh" */
    "    li a1, 0\n"            /* argv = 0,lab2 忽略 */
    "    ecall\n"
    "    # exec 不应返回;失败则 exit(1)\n"
    "    li a7, 2\n"            /* SYS_exit */
    "    li a0, 1\n"
    "    ecall\n"
    "    j .\n"
    "\n"
    "name:\n"
    "    .asciz \"sh\"\n"
);

void
main(void)
{
    consoleinit();

    /* stvec 切换协议(lab3 §2.2):内核执行期间 stvec 一律指向 kernelvec;
     * 仅 usertrapret 返回用户态前才切到 uservec。
     * 尽早设置:此后 S 态任何陷入都进 kerneltrap 而非跳 0 地址。 */
    w_stvec((uint64)kernelvec);

    /* lab1 回归:启动首行打印 banner(协议 0:明文 + 换行)
     * 内容 = 学号十进制 + (学号 % 97) 的小写十六进制,如 "2024302111353 0x2e" */
    printf("%d %x\n", (long)COURSE_SID, (uint64)(COURSE_SID % 97));

    /* lab3 阶段一:物理页分配器初始化(位图,升序,配额=总页数-LAB3_MEMCAP_DELTA) */
    kinit();

#if LAB3_KALLOC_SELFTEST
    /* 阶段一临时验证(验证通过后把上面的宏置 0):
     *   1) 分配 3 页:4KB 对齐、严格升序、内容已清零;
     *   2) 释放中间页再分配:升序语义下应重新拿到该最低空闲页;
     *   3) 计数守恒:全部释放后 used 回到基线;
     *   4) 耗尽:持续分配恰好在 cap 次后返回 0,且内核不 panic;
     *      随后全部释放,used 再次守恒。 */
    {
        int ok = 1;
        uint64 baseline = kpages_used();

        char *p1 = kalloc(), *p2 = kalloc(), *p3 = kalloc();
        if (!p1 || !p2 || !p3) ok = 0;
        if (ok && (((uint64)p1 & 0xFFF) || ((uint64)p2 & 0xFFF) || ((uint64)p3 & 0xFFF)))
            ok = 0;                                  /* 对齐 */
        if (ok && !(p1 < p2 && p2 < p3))
            ok = 0;                                  /* 升序 */
        if (ok)
            for (int i = 0; i < PGSIZE; i++)
                if (p1[i] || p2[i] || p3[i]) { ok = 0; break; }   /* 清零 */

        kfree(p2);
        char *p4 = kalloc();
        if (p4 != p2) ok = 0;                        /* 升序:重发最低空闲页 */

        kfree(p1); kfree(p3); kfree(p4);
        if (kpages_used() != baseline) ok = 0;       /* 守恒 */

        static void *hold[(PHYSTOP - KERNBASE) / PGSIZE];
        uint64 n = 0;
        void *q;
        while ((q = kalloc()) != 0)
            hold[n++] = q;
        if (n != kpages_cap()) ok = 0;               /* 恰好触顶 */
        if (kalloc() != 0) ok = 0;                   /* 触顶后仍返回 0 */
        while (n > 0)
            kfree(hold[--n]);
        if (kpages_used() != baseline) ok = 0;       /* 再次守恒 */

        printf("kalloc selftest: %s (cap=%d)\n", ok ? "PASS" : "FAIL", kpages_cap());
        if (!ok) { intr_off(); while (1) {} }
    }
#endif

#if LAB3_VM_SELFTEST
    /* 阶段二临时验证(验证通过后把上面的宏置 0):
     *  ① 独立根页表映射 3 页,walk 反查 PA 与权限位,空洞无有效叶子;
     *  ② 耗空配额至剩 2 页空闲,发起需 3 个中间页的 4MB 映射,
     *     验证失败完整回滚(used 回基线、目标区无残留、数据页所有权未被夺);
     *  ③ unmappings(do_free=1) 回收 3 数据页 + 2 中间页,计数守恒。
     * 注意 walk 语义:中间表存在时返回叶子 PTE 指针,调用方查 PTE_V。 */
    {
        uint64 base0 = kpages_used();

        /* ① walk + mappages 基本语义 */
        int ok = 1;
        pagetable_t pt = (pagetable_t)kalloc();
        char *d1 = kalloc(), *d2 = kalloc(), *d3 = kalloc();
        if (!pt || !d1 || !d2 || !d3) ok = 0;
        if (ok) {
            if (mappages(pt, 0x1000, PGSIZE, (uint64)d1, PTE_R | PTE_W | PTE_U) != 0) ok = 0;
            if (mappages(pt, 0x2000, PGSIZE, (uint64)d2, PTE_R | PTE_W | PTE_U) != 0) ok = 0;
            if (mappages(pt, 0x3000, PGSIZE, (uint64)d3, PTE_R | PTE_X | PTE_U) != 0) ok = 0;
            pte_t *e1 = walk(pt, 0x1000, 0);
            pte_t *e3 = walk(pt, 0x3000, 0);
            pte_t *e4 = walk(pt, 0x4000, 0);
            if (!e1 || (*e1 & PTE_V) == 0 || PTE2PA(*e1) != (uint64)d1) ok = 0;
            if (!e1 || (*e1 & (PTE_R | PTE_W | PTE_U)) != (PTE_R | PTE_W | PTE_U)) ok = 0;
            if (e1 && (*e1 & PTE_X)) ok = 0;                       /* d1 无 X */
            if (!e3 || PTE2PA(*e3) != (uint64)d3) ok = 0;
            if (e3 && ((*e3 & PTE_X) == 0 || (*e3 & PTE_W))) ok = 0; /* d3 有 X 无 W */
            if (e4 && (*e4 & PTE_V)) ok = 0;                       /* 空洞无叶子 */
            if (kpages_used() != base0 + 6) ok = 0;  /* 根+L1+L0+3 数据页 */
        }
        printf("vm selftest 1/3 map+walk: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ② 耗空至剩 2 页空闲,验证大区间映射失败完整回滚 */
        ok = 1;
        uint64 before = kpages_used();
        {
            static void *hold[(PHYSTOP - KERNBASE) / PGSIZE];
            uint64 n = 0;
            while (kpages_free() > 3) {
                void *q = kalloc();
                if (!q) { ok = 0; break; }
                hold[n++] = q;
            }
            char *victim = kalloc();        /* 调用方数据页,回滚后所有权应保留 */
            uint64 v_used = kpages_used();  /* victim 分配后的占用基线 */
            if (!victim || kpages_free() != 2) ok = 0;
            if (ok) {
                /* VA 1GB 处映 4MB:需 L1+2×L0 共 3 中间页,仅 2 页可分配,
                 * 第一个 2MB 映满后 L0#2 分配失败 -> 回滚 */
                if (mappages(pt, 0x40000000, 0x400000, (uint64)victim, PTE_R | PTE_W) != -1)
                    ok = 0;
                if (walk(pt, 0x40000000, 0) != 0) ok = 0;        /* L1 已回收,无残留 */
                if (walk(pt, 0x40200000, 0) != 0) ok = 0;
                if (kpages_used() != v_used) ok = 0;             /* 中间页全回收 */
                kfree(victim);   /* 若回滚误释放数据页,此处 double free 会 panic */
            }
            while (n > 0)
                kfree(hold[--n]);
            if (kpages_used() != before) ok = 0;                 /* 守恒 */
        }
        printf("vm selftest 2/3 rollback: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ③ unmappings:回收数据页并 prune 变空中间页 */
        ok = 1;
        unmappings(pt, 0x1000, 3 * PGSIZE, 1);
        if (walk(pt, 0x1000, 0) != 0) ok = 0;
        if (walk(pt, 0x3000, 0) != 0) ok = 0;
        if (kpages_used() != base0 + 1) ok = 0;                  /* 仅剩根页表 */
        unmappings(pt, 0x8000, 3 * PGSIZE, 1);                   /* 空洞区间:无副作用 */
        if (kpages_used() != base0 + 1) ok = 0;
        kfree(pt);
        if (kpages_used() != base0) ok = 0;                      /* 完全守恒 */
        printf("vm selftest 3/3 unmap+prune: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }
    }
#endif

    printf("=== lab2 stage7: run sh ===\n");

    plicinit();
    printf("plic inited (UART0 IRQ=%d enabled)\n", UART0_IRQ);

    w_sie(r_sie() | SIE_SEIE);

    /* 内核页表:恒等映射全部 RAM + UART/PLIC + TRAMPOLINE;
     * 须在首个 allocproc 之前建好(kstack_map/uvmcreate 依赖它) */
    uint64 trampoline_pa = PGROUNDDOWN((uint64)trampoline);
    pagetable_t kpgtbl = kvminit(trampoline_pa);
    printf("kpgtbl=%x, switching satp...\n", (uint64)kpgtbl);
    kvminithart(kpgtbl);

#if LAB3_UVM_SELFTEST
    /* 阶段四临时验证(验证通过后把上面的宏置 0):
     * ① uvmload 布局与段权限:伪造 3 页镜像(wstart=0x2000,memsz=filesz+37),
     *    检查代码页 RXU 无 W、数据页 RWU 无 X、bss 清零、guard 空洞、栈顶 sz/sp;
     * ② uvmcopy:物理隔离(PA 不同、内容同、权限同),写副本不影响原页;
     * ③ copyin/copyout/copyinstr:跨表受控拷贝,代码页与 guard 空洞写失败;
     * ④ uvmfree 后 kpages_used 回基线;
     * ⑤ _uprog_meta:wstart 页对齐(pagepad 生效)、filesz ≤ memsz、0 < wstart ≤ memsz。 */
    {
        int ok = 1;
        uint64 base0 = kpages_used();

        /* ① 伪造镜像:2 页代码/只读 + 100 字节数据 + 37 字节 bss
         * (镜像放 bss 静态数组:kalloc 单页装不下 2 页+且页间不保证连续) */
        static char img[3 * PGSIZE];
        uint64 filesz = 2 * PGSIZE + 100;
        for (uint64 i = 0; i < filesz; i++)
            img[i] = (char)(i * 7 + 1);
        uint64 memsz = filesz + 37;

        pagetable_t t1 = uvmcreate();
        uint64 sz1 = 0, sp1 = 0;
        if (!t1 || uvmload(t1, img, filesz, 2 * PGSIZE, memsz, &sz1, &sp1) != 0)
            ok = 0;
        uint64 top = PGROUNDUP(memsz);
        uint64 stack_va = top + LAB3_GUARD_PAGES * PGSIZE;
        if (ok) {
            pte_t *e0 = walk(t1, 0, 0);              /* 代码页 */
            pte_t *e2 = walk(t1, 2 * PGSIZE, 0);     /* 数据页 */
            pte_t *es = walk(t1, stack_va, 0);       /* 栈页 */
            pte_t *eg = walk(t1, top, 0);            /* guard 首页(空洞) */
            if (!e0 || (*e0 & (PTE_R | PTE_X | PTE_U)) != (PTE_R | PTE_X | PTE_U)) ok = 0;
            if (e0 && (*e0 & PTE_W)) ok = 0;                       /* 代码页无 W */
            if (!e2 || (*e2 & (PTE_R | PTE_W | PTE_U)) != (PTE_R | PTE_W | PTE_U)) ok = 0;
            if (e2 && (*e2 & PTE_X)) ok = 0;
            if (!es || (*es & (PTE_R | PTE_W | PTE_U)) != (PTE_R | PTE_W | PTE_U)) ok = 0;
            if (eg && (*eg & PTE_V)) ok = 0;                       /* guard 无映射 */
            if (sz1 != stack_va + PGSIZE || sp1 != sz1) ok = 0;
            if (e2) {
                char *dpa = (char *)PTE2PA(*e2);
                for (uint64 i = 100; i < 137; i++)                 /* bss 清零 */
                    if (dpa[i] != 0) { ok = 0; break; }
                for (uint64 i = 0; i < 100; i++)                   /* 内容一致 */
                    if (dpa[i] != (char)((2 * PGSIZE + i) * 7 + 1)) { ok = 0; break; }
            }
        }
        printf("uvm selftest 1/5 layout: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ② uvmcopy 物理隔离 */
        pagetable_t t2 = uvmcreate();
        if (!t2 || uvmcopy(t1, t2, sz1) != 0) ok = 0;
        if (ok) {
            pte_t *a  = walk(t1, 2 * PGSIZE, 0);
            pte_t *b  = walk(t2, 2 * PGSIZE, 0);
            pte_t *tb = walk(t2, 0, 0);
            pte_t *g2 = walk(t2, top, 0);
            if (!a || !b || PTE2PA(*a) == PTE2PA(*b)) ok = 0;      /* 物理隔离 */
            if (a && b && PTE_FLAGS(*a) != PTE_FLAGS(*b)) ok = 0;  /* 权限保留 */
            if (tb && (PTE_FLAGS(*tb) & PTE_W)) ok = 0;            /* 副本代码页仍无 W */
            if (g2 && (*g2 & PTE_V)) ok = 0;                       /* 副本 guard 仍空洞 */
            if (a && b) {
                char *pa = (char *)PTE2PA(*a), *pb = (char *)PTE2PA(*b);
                for (int i = 0; i < 100; i++)
                    if (pa[i] != pb[i]) { ok = 0; break; }         /* 内容一致 */
                pb[0] = (char)~pa[0];
                if (pa[0] == pb[0]) ok = 0;                        /* 写副本不改原件 */
            }
        }
        printf("uvm selftest 2/5 fork-copy: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ③ copyin/copyout/copyinstr */
        {
            char kbuf[32], rbuf[32];
            for (int i = 0; i < 32; i++) { kbuf[i] = (char)(0x40 + i); rbuf[i] = 0; }
            if (copyout(t1, 2 * PGSIZE, kbuf, 32) != 0) ok = 0;
            if (copyin(t1, rbuf, 2 * PGSIZE, 32) != 0) ok = 0;
            for (int i = 0; i < 32; i++)
                if (rbuf[i] != (char)(0x40 + i)) ok = 0;
            if (copyout(t1, 0, kbuf, 32) == 0) ok = 0;             /* 代码页无 W:失败 */
            if (copyout(t1, top, kbuf, 32) == 0) ok = 0;           /* guard 空洞:失败 */
            pte_t *ed = walk(t1, 2 * PGSIZE, 0);
            if (ed) {
                char *dpa = (char *)PTE2PA(*ed);
                dpa[0] = 'o'; dpa[1] = 'k'; dpa[2] = 0;
                char sbuf[8];
                for (int i = 0; i < 8; i++) sbuf[i] = 0;
                if (copyinstr(t1, sbuf, 2 * PGSIZE, 8) != 0) ok = 0;
                if (sbuf[0] != 'o' || sbuf[1] != 'k' || sbuf[2] != 0) ok = 0;
            } else ok = 0;
        }
        printf("uvm selftest 3/5 copyin/out: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ④ uvmfree 回收守恒 */
        uvmfree(t1, sz1);
        uvmfree(t2, sz1);
        if (kpages_used() != base0) ok = 0;
        printf("uvm selftest 4/5 free: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ⑤ 构建期元数据自洽 */
        {
            extern char _uprog_table[];
            extern uint64 _uprog_meta[];
            uint64 curp = (uint64)_uprog_table;
            int i = 0, n = 0;
            for (;;) {
                uint64 s = *(uint64 *)curp;
                uint64 e = *(uint64 *)(curp + 8);
                if (s == 0 && e == 0) break;
                uint64 ws = _uprog_meta[i * 2], me = _uprog_meta[i * 2 + 1];
                if ((ws & (PGSIZE - 1)) != 0) ok = 0;   /* pagepad 对齐生效 */
                if (ws == 0 || ws > me) ok = 0;
                if (e - s > me) ok = 0;
                curp = (e + 7) & ~7UL;
                i++; n++;
            }
            if (n == 0) ok = 0;
            printf("uvm selftest 5/5 meta(%d progs): %s\n", n, ok ? "PASS" : "FAIL");
            if (!ok) { intr_off(); while (1) {} }
        }
    }
#endif

#if LAB3_COPY_SELFTEST
    /* 阶段五临时验证(验证通过后把上面的宏置 0 或删除本块):
     * 用一页数据页(S)与两页代码/只读页(P0/P1)搭建最小用户空间,
     * 覆盖 copyin/copyout/copyinstr 的跨页拼接、权限拒绝、空洞与上界检查。
     * 页面物理地址从 PTE 反查,确保读写的确实是映射出去的那一页。 */
    {
        int ok = 1;
        uint64 base0 = kpages_used();

        pagetable_t t = uvmcreate();
        char *spa = (char *)kalloc();                    /* 数据页:可写 */
        char *spb = (char *)kalloc();                    /* 数据页2:跨页拼接 */
        char *p0  = (char *)kalloc();                    /* 代码页0:RX */
        char *p1  = (char *)kalloc();                    /* 代码页1:RX */
        if (!t || !spa || !spb || !p0 || !p1) ok = 0;
        if (ok && mappages(t, 0x0000, PGSIZE, (uint64)p0,
                           PTE_R | PTE_X | PTE_U | PTE_A) != 0) ok = 0;
        if (ok && mappages(t, 0x1000, PGSIZE, (uint64)p1,
                           PTE_R | PTE_X | PTE_U | PTE_A) != 0) ok = 0;
        uint64 sva = 0x2000 + LAB3_GUARD_PAGES * PGSIZE;      /* 数据页 VA */
        uint64 gva = 0x2000;                                  /* guard 首 VA */
        if (ok && mappages(t, sva, PGSIZE, (uint64)spa,
                           PTE_R | PTE_W | PTE_U | PTE_A | PTE_D) != 0) ok = 0;
        if (ok && mappages(t, sva + PGSIZE, PGSIZE, (uint64)spb,
                           PTE_R | PTE_W | PTE_U | PTE_A | PTE_D) != 0) ok = 0;

        char kbuf[16], rbuf[16];

        /* ① copyout/copyin 基本往返 */
        if (ok) {
            for (int i = 0; i < 16; i++) { kbuf[i] = (char)(0x30 + i); rbuf[i] = 0; }
            if (copyout(t, sva, kbuf, 16) != 0) ok = 0;
            if (copyin(t, rbuf, sva, 16) != 0) ok = 0;
            for (int i = 0; i < 16; i++)
                if (rbuf[i] != (char)(0x30 + i)) { ok = 0; break; }
        }
        printf("copy selftest 1/5 basic: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ② 跨页:从第一页尾部 8 字节起写 16 字节,跨入第二页;
         *    回读一致证明逐页拼接正确。再越出第二页 1 字节 → 必须失败。 */
        {
            int c1 = 1, c2 = 1;
            if (ok) {
                for (int i = 0; i < 16; i++) kbuf[i] = (char)(0xA0 + i);
                if (copyout(t, sva + PGSIZE - 8, kbuf, 16) != 0) c1 = 0;
                if (copyin(t, rbuf, sva + PGSIZE - 8, 16) != 0) c1 = 0;
                for (int i = 0; i < 16; i++)
                    if (rbuf[i] != (char)(0xA0 + i)) c1 = 0;
                /* 越出第二页尾 1 字节:第三页未映射,必须失败 */
                if (copyout(t, sva + 2 * PGSIZE - 4, kbuf, 8) == 0) c2 = 0;
                if (copyin(t, rbuf, sva + 2 * PGSIZE - 4, 8) == 0) c2 = 0;
            } else { c1 = c2 = 0; }
            if (!c1 || !c2) ok = 0;
        }
        printf("copy selftest 2/5 cross-page: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ③ 权限:代码页 copyout 拒绝、copyin 允许;guard 与未映射地址均拒绝 */
        {
            int a = 1, b = 1, c = 1, d = 1;
            if (ok) {
                if (copyout(t, 0x0000, kbuf, 16) == 0) a = 0;    /* 代码页无 W */
                if (copyin(t, rbuf, 0x0000, 16) != 0) b = 0;     /* 代码页可读 */
                if (copyout(t, gva, kbuf, 16) == 0) c = 0;       /* guard 空洞 */
                if (copyin(t, rbuf, gva, 16) == 0) c = 0;
                if (copyout(t, 0x900000, kbuf, 16) == 0) d = 0;  /* 高地址未映射 */
                if (copyin(t, rbuf, 0x900000, 16) == 0) d = 0;
            } else { a = b = c = d = 0; }
            if (!a || !b || !c || !d) ok = 0;
        }
        printf("copy selftest 3/5 perms/holes: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ④ 上界与溢出:MAXVA 附近与加法回绕必须拒绝,零长度必须成功 */
        {
            int a = 1, b = 1, c = 1, d = 1;
            if (ok) {
                if (copyin(t, rbuf, MAXVA, 16) == 0) a = 0;              /* VA=MAXVA 非法 */
                if (copyout(t, MAXVA - 4, kbuf, 16) == 0) b = 0;         /* 跨界 MAXVA 非法 */
                if (copyin(t, rbuf, ~0UL - 4, 16) == 0) c = 0;           /* 回绕非法 */
                if (copyout(t, sva, kbuf, 0) != 0) d = 0;               /* 零长度成功 */
                if (copyin(t, rbuf, sva, 0) != 0) d = 0;
                if (copyinstr(t, rbuf, MAXVA, 8) == 0) d = 0;           /* copyinstr 越界 */
            } else { a = b = c = d = 0; }
            if (!a || !b || !c || !d) ok = 0;
        }
        printf("copy selftest 4/5 bounds: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ⑤ copyinstr:跨页拼接(字符串起始于页尾、NUL 在下一页);
         *    无 NUL 超长 → -1。用代码页 P0/P1 两页构造。 */
        {
            int a = 1, b = 1;
            if (ok) {
                /* P0 页尾写 'x','y',P1 页首写 'z',0 */
                p0[PGSIZE - 2] = 'x';
                p0[PGSIZE - 1] = 'y';
                p1[0] = 'z';
                p1[1] = '\0';
                for (int i = 0; i < 16; i++) rbuf[i] = 0x7f;
                if (copyinstr(t, rbuf, PGSIZE - 2, 16) != 0) a = 0;     /* 跨 P0→P1 */
                if (rbuf[0] != 'x' || rbuf[1] != 'y' || rbuf[2] != 'z' || rbuf[3] != 0)
                    a = 0;
                /* 无 NUL:P1 全非 0,限长 4 → 失败 */
                p1[0] = 'q'; p1[1] = 'q'; p1[2] = 'q'; p1[3] = 'q'; p1[4] = 'q';
                if (copyinstr(t, rbuf, PGSIZE, 4) == 0) b = 0;
            } else { a = b = 0; }
            if (!a || !b) ok = 0;
        }
        printf("copy selftest 5/5 copyinstr: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        uvmfree(t, sva + 2 * PGSIZE);
        if (kpages_used() != base0) {
            printf("copy selftest: free not conserved\n");
            intr_off(); while (1) {}
        }
    }
#endif

#if LAB3_SBRK_SELFTEST
    /* 阶段六临时验证(验证通过后把上面的宏置 0):
     * ① uvmalloc 增长 3 页:叶子 PTE 为 RWU,且经 copyout/copyin 往返一致;
     * ② uvmdealloc 收缩到 1 页:尾部两页 PTE 清除(物理页已回收),首页保留;
     * ③ 上界:增长越过 TRAPFRAME 必须立即失败,不产生映射与占用;
     * ④ 回滚:留 2 页空闲后请求 4 页,第 3 页 kalloc 失败,
     *    前 2 页映射与中间页必须完整回滚,used 守恒。 */
    {
        int ok = 1;
        uint64 base0 = kpages_used();

        pagetable_t t = uvmcreate();
        if (!t) ok = 0;

        /* ① EAGER 增长 */
        if (ok && uvmalloc(t, 0, 3 * PGSIZE) != 0) ok = 0;
        if (ok) {
            char w[8] = {1, 2, 3, 4, 5, 6, 7, 8}, r[8];
            for (int i = 0; i < 3 && ok; i++) {
                uint64 va = (uint64)i * PGSIZE;
                pte_t *e = walk(t, va, 0);
                if (!e || (*e & PTE_V) == 0) { ok = 0; break; }
                if ((*e & (PTE_R | PTE_W | PTE_U)) !=
                    (PTE_R | PTE_W | PTE_U)) ok = 0;
                if (copyout(t, va, w, 8) != 0) ok = 0;
                for (int j = 0; j < 8; j++) r[j] = 0;
                if (copyin(t, r, va, 8) != 0) ok = 0;
                for (int j = 0; j < 8; j++)
                    if (r[j] != w[j]) ok = 0;
            }
        }
        printf("sbrk selftest 1/4 grow: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ② 收缩:只保留首页 */
        if (uvmdealloc(t, 3 * PGSIZE, PGSIZE) != 0) ok = 0;
        {
            pte_t *e0 = walk(t, 0, 0);
            pte_t *e1 = walk(t, PGSIZE, 0);
            pte_t *e2 = walk(t, 2 * PGSIZE, 0);
            if (!e0 || (*e0 & PTE_V) == 0) ok = 0;
            if (e1 && (*e1 & PTE_V)) ok = 0;
            if (e2 && (*e2 & PTE_V)) ok = 0;
        }
        printf("sbrk selftest 2/4 shrink: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ③ 上界:越过 TRAPFRAME 立即失败,零副作用 */
        {
            uint64 before = kpages_used();
            if (uvmalloc(t, PGSIZE, TRAPFRAME + PGSIZE) == 0) ok = 0;
            if (kpages_used() != before) ok = 0;
            pte_t *e = walk(t, PGSIZE, 0);
            if (e && (*e & PTE_V)) ok = 0;
        }
        printf("sbrk selftest 3/4 bound: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }

        /* ④ 配额不足:分配中途失败必须完整回滚 */
        {
            uint64 steady = kpages_used();
            static void *hold[(PHYSTOP - KERNBASE) / PGSIZE];
            uint64 n = 0;
            while (kpages_free() > 2) {
                void *q = kalloc();
                if (!q) break;
                hold[n++] = q;
            }
            uint64 before = kpages_used();
            if (uvmalloc(t, PGSIZE, PGSIZE + 4 * PGSIZE) == 0) ok = 0;
            if (kpages_used() != before) ok = 0;         /* 已分配部分全回滚 */
            pte_t *e = walk(t, PGSIZE, 0);
            if (e && (*e & PTE_V)) ok = 0;
            while (n > 0)
                kfree(hold[--n]);
            if (kpages_used() != steady) ok = 0;         /* 释放后守恒 */
        }
        uvmfree(t, PGSIZE);
        if (kpages_used() != base0) ok = 0;
        printf("sbrk selftest 4/4 rollback: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }
    }
#endif

    /* 首进程:allocproc 建用户页表 + TRAPFRAME 同址 + KSTACK 内核栈 */
    struct proc *p = allocproc();
    if (!p) { printf("allocproc failed\n"); while (1) {} }
    printf("proc pid=%d\n", p->pid);

    extern struct proc *curproc;
    curproc = p;

    /* initcode(_user_test)装配进首进程用户页表:
     * VA0 一页 RXU 代码(含 "sh" 字符串),LAB3_GUARD_PAGES 个保护页空洞,
     * 再一页 RWU 用户栈;堆自栈顶向上。与 exec 装载布局一致。 */
    uint64 code_page = PGROUNDDOWN((uint64)_user_test);
    uint64 code_off  = (uint64)_user_test - code_page;
    char *cpage = (char *)kalloc();
    char *spage = (char *)kalloc();
    if (!cpage || !spage) { printf("initcode: no mem\n"); while (1) {} }
    for (uint64 i = code_off; i < PGSIZE; i++)
        cpage[i] = ((char *)code_page)[i];
    uint64 istack_va = PGSIZE + LAB3_GUARD_PAGES * PGSIZE;
    if (mappages(p->pagetable, 0, PGSIZE, (uint64)cpage,
                 PTE_R | PTE_X | PTE_U | PTE_A) != 0) {
        printf("initcode: map text failed\n"); while (1) {}
    }
    if (mappages(p->pagetable, istack_va, PGSIZE, (uint64)spage,
                 PTE_R | PTE_W | PTE_U | PTE_A | PTE_D) != 0) {
        printf("initcode: map stack failed\n"); while (1) {}
    }
    p->sz = istack_va + PGSIZE;
    p->trapframe->epc = code_off;
    p->trapframe->sp  = p->sz;

    printf("user_epc=%x sz=%x trampoline=%x tf=%x\n",
           p->trapframe->epc, p->sz, trampoline_pa, (uint64)p->trapframe);

#if LAB3_KVM_SELFTEST
    /* 阶段三临时验证(验证通过后把上面的宏置 0):
     * ① 恒等映射精确到 PHYSTOP:root[2] 是非叶子中间项(不再是 1GB 超级页),
     *    L1[0]/L1[63] 为 2MB 超级页叶子且 PA/权限正确,L1[64](PHYSTOP 起)无效;
     * ② UART / PLIC(SCLAIM 页)/ TRAMPOLINE / TRAPFRAME 映射与权限;
     * ③ stvec 在内核态指向 kernelvec;
     * ④ satp 切换后恒等映射下新 kalloc 页可读写。 */
    {
        int ok = 1;
        pte_t *r2 = walklevel(kpgtbl, KERNBASE, 2);
        if (!r2 || (*r2 & PTE_V) == 0) ok = 0;
        if (r2 && (*r2 & (PTE_R | PTE_W | PTE_X))) ok = 0;    /* 非叶子 */

        pte_t *l1a = walklevel(kpgtbl, KERNBASE, 1);
        pte_t *l1b = walklevel(kpgtbl, PHYSTOP - PGSIZE, 1);
        pte_t *l1c = walklevel(kpgtbl, PHYSTOP, 1);
        uint64 rwxv = PTE_R | PTE_W | PTE_X | PTE_V;
        if (!l1a || PTE2PA(*l1a) != KERNBASE) ok = 0;
        if (l1a && (*l1a & rwxv) != rwxv) ok = 0;
        if (!l1b || PTE2PA(*l1b) != PHYSTOP - 0x200000) ok = 0;  /* 末 2MB 槽 */
        if (l1c && (*l1c & PTE_V)) ok = 0;                       /* PHYSTOP 之外无映射 */

        pte_t *eu = walk(kpgtbl, UART0, 0);
        if (!eu || (*eu & PTE_V) == 0 || PTE2PA(*eu) != UART0) ok = 0;
        if (eu && (*eu & (PTE_R | PTE_W)) != (PTE_R | PTE_W)) ok = 0;
        pte_t *ep = walk(kpgtbl, PLIC + 0x201004, 0);            /* SCLAIM 所在 */
        if (!ep || (*ep & PTE_V) == 0) ok = 0;
        pte_t *et = walk(kpgtbl, TRAMPOLINE, 0);
        if (!et || PTE2PA(*et) != trampoline_pa) ok = 0;
        if (et && (*et & (PTE_R | PTE_X)) != (PTE_R | PTE_X)) ok = 0;
        pte_t *ef = walk(kpgtbl, TRAPFRAME, 0);
        if (!ef || PTE2PA(*ef) != trapframe_pa) ok = 0;
        if (ef && (*ef & (PTE_R | PTE_W)) != (PTE_R | PTE_W)) ok = 0;

        if (r_stvec() != (uint64)kernelvec) ok = 0;              /* stvec 协议 */

        char *pg = (char *)kalloc();
        if (!pg) ok = 0;
        if (pg) {
            pg[0] = 0x5A;
            pg[PGSIZE - 1] = 0xA5;
            if (pg[0] != 0x5A || pg[PGSIZE - 1] != (char)0xA5) ok = 0;
            kfree(pg);
        }

        printf("kvm selftest: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) { intr_off(); while (1) {} }
    }
#endif

    /* 阶段四:独立用户页表后内核经 copyin/copyout 访问用户内存,
     * 不再需要 SSTATUS_SUM(内核不解引用用户 VA),移除 lab2 的 SUM 设置。 */

    printf("satp OK, going to U mode...\n");
    printf("_user_test: exec(\"sh\", 0) -> shell\n");

    usertrapret();

    printf("ERROR: returned from usertrapret\n");
    while (1) {}
}
