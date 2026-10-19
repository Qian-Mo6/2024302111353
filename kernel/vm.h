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

/* ============ lab3 阶段四 · 用户地址空间接口 ============
 * 布局(memlayout.h 注释图):代码/只读在低地址(RXU)、数据/bss(RWU)、
 * LAB3_GUARD_PAGES 个保护页(空洞不映射)、一页用户栈(RWU)、堆自栈顶向上。
 * TRAPFRAME 在每进程用户页表同址映射(权限 RW,无 U:仅 S 态 trampoline 访问);
 * TRAMPOLINE 与内核页表同址(RX)。 */
pagetable_t kvminit(uint64 trampoline_pa);
pagetable_t uvmcreate(void);                       /* 根页表 + TRAMPOLINE 同址映射 */
int    uvmload(pagetable_t pt, char *img, uint64 filesz,
               uint64 wstart, uint64 memsz, uint64 *szp, uint64 *spp);
int    uvmcopy(pagetable_t oldpt, pagetable_t newpt, uint64 sz);
void   uvmfree(pagetable_t pt, uint64 sz);
int    copyin(pagetable_t pt, char *dst, uint64 srcva, uint64 len);
int    copyout(pagetable_t pt, uint64 dstva, char *src, uint64 len);
int    copyinstr(pagetable_t pt, char *dst, uint64 srcva, uint64 max);

/* 进程内核栈:内核页表 KSTACK(idx) 高地址映射(与 lab0 快照图一致),
 * 下方留一页不映射作溢出保护。kstack_map 返回栈顶 VA,失败返回 0。 */
uint64 kstack_map(int idx);
void   kstack_unmap(int idx);

#endif // VM_H
