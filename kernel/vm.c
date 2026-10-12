/* ============ lab2 阶段一 · vm.c 最小页表 ============
 * 目的:建立一个最小页表,让 trampoline 能工作
 *   - 恒等映射所有物理 RAM (0x80000000 ~ PHYSTOP)
 *   - 映射 TRAMPOLINE VA → trampoline 物理页
 *   - 映射 TRAPFRAME VA → trapframe 物理页
 *
 * 注意:完整页表机制是 lab3 的内容,这里只做最小化实现
 *       lab2 不做内存分配(kalloc),用静态数组 + bump pointer
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"

/* lab2 简化:QEMU 需要 A/D 位 */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

/* 静态页分配器:lab2 简化,用一个大数组 + bump pointer */
#define PAGE_POOL_PAGES 256   /* 预留 256 页 = 1MB 给页表 */
static char page_pool[PAGE_POOL_PAGES * PGSIZE] __attribute__((aligned(PGSIZE)));
static uint page_pool_idx = 0;

static void *kalloc_page(void) {
    if (page_pool_idx >= PAGE_POOL_PAGES)
        return 0;
    void *p = &page_pool[page_pool_idx * PGSIZE];
    page_pool_idx++;
    /* 清零 */
    for (int i = 0; i < PGSIZE / 8; i++)
        ((uint64 *)p)[i] = 0;
    return p;
}

/* walk:遍历页表,返回 va 对应的 PTE 指针
 * alloc=1 时,缺页表页则分配 */
static pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (*pte & PTE_V) {
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc) return 0;
            pagetable = (pagetable_t)kalloc_page();
            if (!pagetable) return 0;
            *pte = PA2PTE((uint64)pagetable) | PTE_V;
        }
    }
    return &pagetable[PX(0, va)];
}

/* mappages:建立 va~va+size 到 pa 的映射 */
void
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
    uint64 a = PGROUNDDOWN(va);
    uint64 last = PGROUNDDOWN(va + size - 1);
    for (;;) {
        pte_t *pte = walk(pagetable, a, 1);
        *pte = PA2PTE(pa) | perm | PTE_V;
        if (a == last) break;
        a += PGSIZE;
        pa += PGSIZE;
    }
}

/* kvminit:创建内核页表
 * - 恒等映射 RAM
 * - 映射 TRAMPOLINE → trampoline 代码物理页
 * - 映射 TRAPFRAME → 指定 trapframe 物理页
 */
pagetable_t
kvminit(uint64 trampoline_pa, uint64 trapframe_pa, uint64 user_code_pa)
{
    pagetable_t kpgtbl = (pagetable_t)kalloc_page();

    /* 用 1GB superpage 映射 0x80000000~0xBFFFFFFF
     * 0x80000000 是 1GB 对齐,可以直接在 L2 放叶子 PTE */
    kpgtbl[2] = PA2PTE(KERNBASE) | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D | PTE_V;

    /* UART 映射(printf 需要) */
    mappages(kpgtbl, UART0, PGSIZE, UART0, PTE_R | PTE_W | PTE_A | PTE_D);

    /* PLIC 映射(lab2 stage3:devintr 需要 claim/complete 寄存器
     *   PLIC_SCLAIM 在偏移 0x201004,故映射区必须 > 2MB,这里用 4MB 0x400000) */
    mappages(kpgtbl, PLIC, 0x400000, PLIC, PTE_R | PTE_W | PTE_A | PTE_D);

    /* TRAMPOLINE / TRAPFRAME 需要单独映射(在高地址,不在 superpage 范围内) */
    mappages(kpgtbl, TRAMPOLINE, PGSIZE, trampoline_pa, PTE_R | PTE_X | PTE_A | PTE_D);
    mappages(kpgtbl, TRAPFRAME, PGSIZE, trapframe_pa, PTE_R | PTE_W | PTE_A | PTE_D);

    /* 用户代码页:VA 0x0 → user_code_pa,需要 U 位供 U 态执行 */
    mappages(kpgtbl, 0x0, PGSIZE, user_code_pa, PTE_R | PTE_X | PTE_U | PTE_A | PTE_D);

    return kpgtbl;
}

/* kvminithart:把 satp 切到内核页表 */
void
kvminithart(pagetable_t kpgtbl)
{
    w_satp(MAKE_SATP(kpgtbl));
    sfence_vma();
}
