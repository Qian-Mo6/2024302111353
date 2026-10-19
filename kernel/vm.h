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

#endif // VM_H
