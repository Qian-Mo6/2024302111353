/* ============ lab3 阶段一 · 物理页分配器(位图管理) ============
 * 管理范围:[PGROUNDUP(kernel_end), PHYSTOP),4KB 粒度。
 *
 * 数据结构选型(设计笔记要点):
 *   - 升序空闲链表:分配 O(1),但单页释放需按地址有序插入 O(n),
 *     memcap 批量回收数万页时总体 O(n^2),QEMU 下耗时数分钟,不可用;
 *   - 位图:每页 1 bit,128MB 全区间仅 32768 bit = 4KB 元数据;
 *     释放 O(1),分配按字(64bit)跳过满段检索最低空闲位,天然满足
 *     LAB3_ALLOC_ORDER=0(升序)语义。故选位图。
 *
 * 配额:可分配上限 cap = 托管总页数 - LAB3_MEMCAP_DELTA(=9)。
 *   配额耗尽或物理页耗尽时 kalloc 返回 0,由调用方逐级回滚,内核不 panic。
 *
 * 填充策略:
 *   - kfree 填 0xAA 垃圾数据:释放后若仍有悬挂指针读写该页,
 *     重新分配后能尽快暴露脏数据特征,便于排查 use-after-free;
 *   - kalloc 返回前清零:进程间物理页复用必须经过数据隔离,
 *     防止新进程读到上一个进程的残留数据。
 *
 * 并发说明:单核,但 kalloc/kfree 可能被陷入路径(sbrk 等)调用,
 *   位图与计数器的读改写需关 SIE 保护临界区,退出时恢复原状态。
 * ======================================== */
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "course_sid.h"   /* LAB3_ALLOC_ORDER / LAB3_MEMCAP_DELTA */
#include "vm.h"

extern char kernel_end[];   /* kernel.ld 导出的内核镜像末尾(注意非 xv6 的 end) */

void printf(char *fmt, ...);

/* ---- 位图:覆盖整个 RAM 区间 [KERNBASE, PHYSTOP) ---- */
#define RAM_PAGES   ((PHYSTOP - KERNBASE) / PGSIZE)   /* 128MB -> 32768 页 */
#define BM_WORDS    (RAM_PAGES / 64)                  /* 每 uint64 管 64 页 */

static uint64 bm[BM_WORDS];                 /* 0=空闲 1=已占用(含内核镜像) */
static uint64 managed_base;                 /* 托管起始物理地址(页对齐) */
static uint64 first_word;                   /* managed_base 所在的位图字下标 */
static uint64 total_pages;                  /* 托管页总数 */
static uint64 cap_pages;                    /* 配额上限 = total - DELTA */
static uint64 nalloc;                       /* 分配器已发出页数(不含镜像占用) */

/* ---- 最小 memset(内核无 libc) ---- */
static void
kmemset(void *dst, int c, uint64 n)
{
    uint8 *d = (uint8 *)dst;
    uint8 v = (uint8)c;
    /* 按 8 字节批量填充,尾部逐字节 */
    uint64 pat = v * 0x0101010101010101ULL;
    while (n >= 8) { *(uint64 *)d = pat; d += 8; n -= 8; }
    while (n--)    { *d++ = v; }
}

/* ---- 临界区:保存/恢复 SIE ---- */
static uint64
intr_save(void)
{
    uint64 s = r_sstatus();
    intr_off();
    return s;
}
static void
intr_restore(uint64 s)
{
    if (s & SSTATUS_SIE)
        intr_on();
}

/* ---- 位图原语:idx = 全 RAM 区间内的页号 ---- */
#define PA2IDX(pa)  (((uint64)(pa) - KERNBASE) >> 12)
#define IDX2PA(i)   (KERNBASE + ((uint64)(i) << 12))

static inline int
bm_test(uint64 idx)
{
    return (bm[idx >> 6] >> (idx & 63)) & 1;
}
static inline void
bm_set(uint64 idx)
{
    bm[idx >> 6] |= (1ULL << (idx & 63));
}
static inline void
bm_clear(uint64 idx)
{
    bm[idx >> 6] &= ~(1ULL << (idx & 63));
}

static void
kpanic(char *msg)
{
    printf("kalloc panic: %s\n", msg);
    intr_off();
    while (1) {}
}

/* kinit:界定托管区间,初始化位图
 *   [KERNBASE, managed_base) 为内核镜像(含 BSS),在位图中永久置占用;
 *   [managed_base, PHYSTOP) 全部标记空闲。 */
void
kinit(void)
{
    managed_base = PGROUNDUP((uint64)kernel_end);
    total_pages  = (PHYSTOP - managed_base) / PGSIZE;
    cap_pages    = total_pages - LAB3_MEMCAP_DELTA;
    nalloc       = 0;
    first_word   = PA2IDX(managed_base) >> 6;

    for (uint64 i = 0; i < BM_WORDS; i++)
        bm[i] = 0;
    for (uint64 pa = KERNBASE; pa < managed_base; pa += PGSIZE)
        bm_set(PA2IDX(pa));

    printf("kalloc: base=%x total=%d cap=%d (delta=%d order=%s)\n",
           managed_base, total_pages, cap_pages,
           LAB3_MEMCAP_DELTA,
           LAB3_ALLOC_ORDER == 0 ? "asc" :
           LAB3_ALLOC_ORDER == 1 ? "desc" : "mid");
}

/* kalloc:按 LAB3_ALLOC_ORDER=0(升序)分配一个物理页
 * 返回页起始物理地址;配额/物理耗尽返回 0(不 panic)。
 * 严格升序:每次从头扫描,保证总是发出当前最低空闲页。 */
void *
kalloc(void)
{
    uint64 s = intr_save();

    if (nalloc >= cap_pages) {          /* 配额上限:总页数 - DELTA */
        intr_restore(s);
        return 0;
    }

    for (uint64 w = first_word; w < BM_WORDS; w++) {
        uint64 word = bm[w];
        if (word == ~0ULL)              /* 整字 64 页全占用,跳过 */
            continue;
        /* 最低空闲位 -> 升序。rv64gc 无 Zbb,__builtin_ctzll 会转为
         * libgcc 的 __ctzdi2 调用(freestanding 无此库),故手动逐位求 ctz */
        int bit = 0;
        for (uint64 inv = ~word; (inv & 1) == 0; inv >>= 1)
            bit++;
        uint64 idx = (w << 6) + bit;
        uint64 pa  = IDX2PA(idx);
        if (pa >= PHYSTOP)              /* 到达 RAM 末尾 */
            break;
        bm_set(idx);
        nalloc++;
        intr_restore(s);

        kmemset((void *)pa, 0, PGSIZE);     /* 数据隔离:返回前清零 */
        return (void *)pa;
    }

    intr_restore(s);
    return 0;
}

/* kfree:回收一个物理页
 * 校验:非空、4KB 对齐、位于托管区间、无双重释放。 */
void
kfree(void *pa)
{
    if (pa == 0)
        kpanic("free NULL");
    if (((uint64)pa & (PGSIZE - 1)) != 0)
        kpanic("free unaligned");
    if ((uint64)pa < managed_base || (uint64)pa >= PHYSTOP)
        kpanic("free out of range");

    uint64 s   = intr_save();
    uint64 idx = PA2IDX(pa);
    if (!bm_test(idx))
        kpanic("double free");

    kmemset(pa, 0xAA, PGSIZE);          /* junk:暴露 use-after-free */
    bm_clear(idx);
    nalloc--;
    intr_restore(s);
}

/* ---- 统计接口(自测与 dump 用) ---- */
uint64 kpages_total(void) { return total_pages; }
uint64 kpages_cap(void)   { return cap_pages; }
uint64 kpages_used(void)  { return nalloc; }
uint64 kpages_free(void)  { return cap_pages - nalloc; }
