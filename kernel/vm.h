#ifndef VM_H
#define VM_H

#define SBRK_EAGER 1
#define SBRK_LAZY  2

/* ============ lab3 阶段一 · 物理页分配器接口 ============
 * 使用方需先包含 types.h(uint64)。实现见 kalloc.c。 */
void   kinit(void);
void  *kalloc(void);
void   kfree(void *);
uint64 kpages_total(void);
uint64 kpages_cap(void);
uint64 kpages_used(void);
uint64 kpages_free(void);

/* ============ lab3 阶段二 · SV39 页表核心操作接口 ============
 * 使用方需先包含 riscv.h(pte_t/pagetable_t)。实现见 vm.c。 */
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc);
pte_t *walklevel(pagetable_t pagetable, uint64 va, int level);
int    mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm);
void   unmappings(pagetable_t pagetable, uint64 va, uint64 size, int do_free);

#endif // VM_H
