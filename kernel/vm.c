/* ============ lab3 阶段二 · SV39 页表核心操作 ============
 * walk / walklevel / mappages(含回滚) / unmappings / vm_prune
 *
 * 核心不变式(设计笔记要点):
 *   1. mappages 失败必须完整回滚:清除本调用已建叶子映射,并回收本调用
 *      新分配的、已变空的中间页表页;调用方传入的数据物理页所有权仍归
 *      调用方,mappages 绝不代为释放;
 *   2. 页表中不长期存在"全空中间页表页"——故回滚/拆除后变空的中间页
 *      必为本路径所清,可安全回收(prune);
 *   3. 超级页叶子项(R/W/X 任一置位)不是页表页,prune/walklevel 下沉时
 *      必须跳过,绝不能把 1GB/2MB 叶子当页表扫描或回收;
 *   4. 重复映射已有 VA 属于内核 bug(非运行时资源状况),直接 panic。
 *
 * 页表页来源:kalloc()(返回已清零页,满足页表页全 0 要求)。
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "vm.h"

/* QEMU 需要 A/D 位 */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

void printf(char *fmt, ...);

/* 全局内核页表指针:user_remap(lab2 遗留,阶段四移除)用它 */
static pagetable_t g_kpgtbl = 0;

static void
vm_panic(char *msg)
{
    printf("vm panic: %s\n", msg);
    intr_off();
    while (1) {}
}

/* walk:遍历 SV39 三级页表,返回 va 对应的叶子(L0)PTE 指针
 *   alloc=1:缺中间页表页则 kalloc 分配(失败返回 0);
 *   alloc=0:路径不存在则返回 0,不分配。 */
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc) return 0;
            pagetable = (pagetable_t)kalloc();
            if (!pagetable) return 0;
            *pte = PA2PTE((uint64)pagetable) | PTE_V;
        }
    }
    return &pagetable[PX(0, va)];
}

/* walklevel:返回 va 在指定 level 的 PTE 指针(不分配)
 * 路径中断或遇超级页叶子(无法继续下沉)时返回 0。 */
pte_t *
walklevel(pagetable_t pagetable, uint64 va, int level)
{
    for (int l = 2; l > level; l--) {
        pte_t *pte = &pagetable[PX(l, va)];
        if ((*pte & PTE_V) == 0)
            return 0;
        if (*pte & (PTE_R | PTE_W | PTE_X))
            return 0;               /* 超级页叶子,不能再下沉 */
        pagetable = (pagetable_t)PTE2PA(*pte);
    }
    return &pagetable[PX(level, va)];
}

/* table_empty:页表页 512 项全部无效? */
static int
table_empty(pagetable_t pt)
{
    for (int i = 0; i < 512; i++)
        if (pt[i] & PTE_V)
            return 0;
    return 1;
}

#define SZ_2MB  ((uint64)PGSIZE * 512)      /* 一个 L1 项覆盖范围 */
#define SZ_1GB  (SZ_2MB * 512)              /* 一个 L2 项覆盖范围 */

/* vm_prune:回收 [va0, va1) 范围内已变空的中间页表页(自底向上)
 * 只释放"整页全空"的中间页;含有效映射的页不受影响,故对范围外
 * 的既有映射安全。超级页叶子项跳过。 */
static void
vm_prune(pagetable_t pt, uint64 va0, uint64 va1)
{
    if (va1 <= va0)
        return;

    /* 先收 L0 层:逐个 2MB 槽检查其 L0 表 */
    for (uint64 a = va0 & ~(SZ_2MB - 1); a < va1; a += SZ_2MB) {
        pte_t *l1e = walklevel(pt, a, 1);
        if (!l1e || (*l1e & PTE_V) == 0)
            continue;
        if (*l1e & (PTE_R | PTE_W | PTE_X))
            continue;                       /* 2MB 超级页叶子 */
        pagetable_t l0 = (pagetable_t)PTE2PA(*l1e);
        if (table_empty(l0)) {
            kfree((void *)l0);
            *l1e = 0;
        }
    }

    /* 再收 L1 层:逐个 1GB 槽检查其 L1 表 */
    for (uint64 a = va0 & ~(SZ_1GB - 1); a < va1; a += SZ_1GB) {
        pte_t *l2e = walklevel(pt, a, 2);
        if (!l2e || (*l2e & PTE_V) == 0)
            continue;
        if (*l2e & (PTE_R | PTE_W | PTE_X))
            continue;                       /* 1GB 超级页叶子(RAM 恒等映射) */
        pagetable_t l1 = (pagetable_t)PTE2PA(*l2e);
        if (table_empty(l1)) {
            kfree((void *)l1);
            *l2e = 0;
        }
    }
}

/* mappages:建立 [va, va+size) → [pa, pa+size) 的页粒度映射
 * 返回 0 成功;-1 失败(中间页表页分配失败),此时完整回滚:
 *   已建叶子映射全部清除、本调用新分配的中间页表页全部回收,
 *   数据物理页不释放(所有权在调用方)。
 * 重复映射已有 VA:panic(内核 bug)。 */
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
    uint64 start = PGROUNDDOWN(va);
    uint64 last  = PGROUNDDOWN(va + size - 1);
    uint64 a = start;
    for (;;) {
        pte_t *pte = walk(pagetable, a, 1);
        if (pte == 0) {
            /* 回滚:清掉 [start, a) 内本调用建立的叶子映射 */
            for (uint64 v = start; v < a; v += PGSIZE) {
                pte_t *p = walk(pagetable, v, 0);
                if (p)
                    *p = 0;
            }
            /* 回收本调用新分配、已变空的中间页表页;
             * 范围含失败地址 a 所在槽(walk 可能已为它建过中间页) */
            vm_prune(pagetable, start, a + PGSIZE);
            return -1;
        }
        if (*pte & PTE_V)
            vm_panic("mappages: remap");
        *pte = PA2PTE(pa) | perm | PTE_V;
        if (a == last) break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

/* unmappings:拆除 [va, va+size) 的映射
 *   空洞(未映射页)跳过,不报错——用户地址空间允许保护页空洞;
 *   do_free=1 时同时 kfree 数据物理页;
 *   结束后 prune 回收变空的中间页表页。 */
void
unmappings(pagetable_t pagetable, uint64 va, uint64 size, int do_free)
{
    uint64 start = PGROUNDDOWN(va);
    uint64 end   = PGROUNDDOWN(va + size - 1) + PGSIZE;

    for (uint64 a = start; a < end; a += PGSIZE) {
        pte_t *pte = walk(pagetable, a, 0);
        if (pte == 0 || (*pte & PTE_V) == 0)
            continue;                       /* 空洞,跳过 */
        if (PTE_FLAGS(*pte) == PTE_V)
            vm_panic("unmappings: not a leaf");
        if (do_free)
            kfree((void *)PTE2PA(*pte));
        *pte = 0;
    }
    vm_prune(pagetable, start, end);
}

/* user_remap:lab2 遗留——在全局内核页表中重映射某个用户 VA
 * 直接覆盖原有 PTE(若已存在)。被 proc.c fork / syscall.c wait、exec 使用,
 * 阶段四改造为独立用户页表前必须保留可用。 */
void
user_remap(uint64 va, uint64 pa, int perm)
{
    if (!g_kpgtbl) return;
    pte_t *pte = walk(g_kpgtbl, va, 1);
    if (!pte) return;
    *pte = PA2PTE(pa) | perm | PTE_V;
    sfence_vma();
}

/* kvminit:创建内核页表
 * - 恒等映射全部可用 RAM [KERNBASE, PHYSTOP):
 *   64 个 2MB 超级页叶子挂在 root[2] 下的 L1 表。
 *   不再用 lab2 的 1GB 超级页(会映射越出 PHYSTOP 至 0xBFFFFFFF)。
 *   权限统一 RWX 是阶段三过渡方案;段级权限(内核 text RX、rodata R)
 *   的细化留待后续阶段,在设计笔记中说明。
 *   不变式:内核必须经恒等映射直接读写一切"分配出去"的物理页
 *   (页表页、用户数据页、受控拷贝的物理中转)。
 * - 外设必需映射:UART(串口寄存器)、PLIC(claim/complete)。
 *   CLINT 不映射:本内核时钟访问经 rdtime/stimecmp CSR(Sstc 扩展)
 *   进行,不读写 CLINT MMIO 寄存器——保持最小映射面,设计笔记中说明。
 * - TRAMPOLINE 映射在最高地址:跳板页必须与用户页表同址,
 *   uservec/userret 切 satp 前后指令流才能不断档。
 * - TRAPFRAME / 可选的首个用户程序 VA 0x0(user_remap 会覆盖)。
 *
 * sfence.vma 时机:本函数只建表不刷 TLB;刷 TLB 由各切换点负责——
 *   kvminithart 写 satp 后、uservec/userret 换页表前后(trampoline
 *   预置)、user_remap 改 PTE 后。批量建映射由调用方统一刷,避免逐页刷。
 * 任一步失败说明早期内存不足,直接 panic。 */
pagetable_t
kvminit(uint64 trampoline_pa, uint64 trapframe_pa, uint64 user_code_pa)
{
    pagetable_t kpgtbl = (pagetable_t)kalloc();
    if (!kpgtbl)
        vm_panic("kvminit: no root page");

    /* 恒等映射 [KERNBASE, PHYSTOP):root[2] -> L1 表,64 个 2MB 超级页 */
    pagetable_t l1ram = (pagetable_t)kalloc();
    if (!l1ram)
        vm_panic("kvminit: no RAM L1 page");
    kpgtbl[2] = PA2PTE((uint64)l1ram) | PTE_V;
    for (uint64 pa = KERNBASE; pa < PHYSTOP; pa += SZ_2MB)
        l1ram[PX(1, pa)] = PA2PTE(pa) | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_V;

    /* UART 映射(printf 需要) */
    if (mappages(kpgtbl, UART0, PGSIZE, UART0, PTE_R | PTE_W | PTE_A | PTE_D) != 0)
        vm_panic("kvminit: map UART");

    /* PLIC 映射(devintr 需要 claim/complete 寄存器
     *   PLIC_SCLAIM 在偏移 0x201004,故映射区必须 > 2MB,这里用 4MB 0x400000) */
    if (mappages(kpgtbl, PLIC, 0x400000, PLIC, PTE_R | PTE_W | PTE_A | PTE_D) != 0)
        vm_panic("kvminit: map PLIC");

    /* TRAMPOLINE / TRAPFRAME 需要单独映射(在高地址,不在 superpage 范围内) */
    if (mappages(kpgtbl, TRAMPOLINE, PGSIZE, trampoline_pa, PTE_R | PTE_X | PTE_A | PTE_D) != 0)
        vm_panic("kvminit: map TRAMPOLINE");
    if (mappages(kpgtbl, TRAPFRAME, PGSIZE, trapframe_pa, PTE_R | PTE_W | PTE_A | PTE_D) != 0)
        vm_panic("kvminit: map TRAPFRAME");

    /* 用户代码页:VA 0x0 → user_code_pa,需要 U 位供 U 态执行
     * sys_exec 会用 user_remap 覆盖此映射以加载新程序 */
    if (user_code_pa)
        if (mappages(kpgtbl, 0x0, PGSIZE, user_code_pa, PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D) != 0)
            vm_panic("kvminit: map user code");

    /* 保存全局引用供 user_remap 使用 */
    g_kpgtbl = kpgtbl;

    return kpgtbl;
}

/* kvminithart:把 satp 切到内核页表 */
void
kvminithart(pagetable_t kpgtbl)
{
    w_satp(MAKE_SATP(kpgtbl));
    sfence_vma();
}
