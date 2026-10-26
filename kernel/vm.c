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
#include "course_sid.h"   /* LAB3_GUARD_PAGES */
#include "vm.h"

/* QEMU 需要 A/D 位 */
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)

void printf(char *fmt, ...);

/* 全局内核页表指针:kstack_map/kstack_unmap 在内核页表高地址建/拆
 * 进程内核栈映射(KSTACK 宏,与 lab0 快照图一致)。 */
static pagetable_t g_kpgtbl = 0;
/* 跳板页物理地址:kvminit 记录,供 uvmcreate 在每个用户页表同址映射 */
static uint64 g_trampoline_pa = 0;

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
    if (size == 0)
        return;                             /* 空区间:无副作用(uvmfree 允许 sz=0) */
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

/* ============ lab3 阶段四 · 用户地址空间 ============
 * 每进程独立用户页表:代码/只读(RXU,不含 W)在低地址、数据/bss(RWU)、
 * LAB3_GUARD_PAGES 个保护页(空洞,遍历跳过)、一页用户栈(RWU)、
 * 堆自栈顶向上增长。TRAPFRAME 在各用户页表同址映射(RW 无 U,归内核所有);
 * TRAMPOLINE 与内核页表同址(RX,共享同一物理页)。
 *
 * 段权限数据链路:平铺 .bin 不携带权限,构建期从 ELF 提取——
 * Makefile 用 pagepad.o 把 .data 对齐到页边界(代码页与数据页不再同页,
 * 代码页才能真正去掉 W),objdump/nm 取 .data VMA 与 end 符号,
 * 生成 _uprog_meta 表(wstart/memsz)供 uvmload 使用。 */

/* uvmcreate:新建用户页表(仅含 TRAMPOLINE 同址映射) */
pagetable_t
uvmcreate(void)
{
    pagetable_t pt = (pagetable_t)kalloc();
    if (!pt)
        return 0;
    if (mappages(pt, TRAMPOLINE, PGSIZE, g_trampoline_pa,
                 PTE_R | PTE_X | PTE_A) != 0) {
        uvmfree(pt, 0);
        return 0;
    }
    return pt;
}

/* uvmload:向用户页表装载平铺程序镜像并布置栈/保护页
 *   img/filesz:镜像内核地址与文件字节数([0,filesz) 拷入,其余置零=bss/对齐隙)
 *   wstart:可写段(.data)起始 VA(构建期已页对齐;回退情形取 PGROUNDDOWN)
 *   memsz:进程镜像总大小(end 符号,含 bss)
 * 布局:[0, PGROUNDDOWN(wstart)) RXU | [wpg, PGROUNDUP(memsz)) RWU |
 *      LAB3_GUARD_PAGES 保护页(不映射) | 栈页 RWU;堆顶 sz = 栈顶。
 * 返回 0 成功并填 szp/spp 指向的值;-1 失败,已建映射全部回滚(数据页+中间页回收)。 */
int
uvmload(pagetable_t pt, char *img, uint64 filesz,
        uint64 wstart, uint64 memsz, uint64 *szp, uint64 *spp)
{
    if (filesz > memsz)
        return -1;                          /* 元数据不自洽,拒绝装载 */
    uint64 wpg = PGROUNDDOWN(wstart);       /* 可写区首页 */
    uint64 top = PGROUNDUP(memsz);          /* 代码+数据区顶端 */
    if (top == 0 || top + (LAB3_GUARD_PAGES + 1) * PGSIZE > TRAPFRAME)
        return -1;                          /* 越过 TRAPFRAME,拒绝 */

    for (uint64 va = 0; va < top; va += PGSIZE) {
        char *mem = (char *)kalloc();
        if (!mem)
            goto fail;
        /* 页内 [0,filesz) 来自镜像,其余保持 kalloc 清零(bss 与对齐隙) */
        for (uint64 off = 0; off < PGSIZE && va + off < filesz; off++)
            mem[off] = img[va + off];
        int perm = PTE_U | PTE_A;
        if (va < wpg) perm |= PTE_R | PTE_X;         /* 代码/只读页:无 W */
        else          perm |= PTE_R | PTE_W | PTE_D; /* 数据/bss 页 */
        if (mappages(pt, va, PGSIZE, (uint64)mem, perm) != 0) {
            kfree(mem);
            goto fail;
        }
    }

    uint64 stack_va = top + LAB3_GUARD_PAGES * PGSIZE;   /* 保护页空洞不映射 */
    char *stk = (char *)kalloc();
    if (!stk)
        goto fail;
    if (mappages(pt, stack_va, PGSIZE, (uint64)stk,
                 PTE_R | PTE_W | PTE_U | PTE_A | PTE_D) != 0) {
        kfree(stk);
        goto fail;
    }

    *szp = stack_va + PGSIZE;   /* 堆自栈顶向上增长 */
    *spp = stack_va + PGSIZE;   /* 初始栈顶 */
    return 0;

fail:
    /* 空洞跳过语义下,整段 unmappings 可安全回收已建部分 */
    unmappings(pt, 0, top + (LAB3_GUARD_PAGES + 1) * PGSIZE, 1);
    return -1;
}

/* uvmcopy:把 old 的 [0,sz) 逐页深拷贝到 new(每页新物理页,物理隔离)
 * 空洞(guard 页)跳过;权限位原样保留(代码页保持无 W)。
 * trapframe(TRAPFRAME)与跳板(TRAMPOLINE)位于 sz 之上,不在拷贝范围。
 * 返回 0 成功;-1 失败,已拷贝部分由调用方 uvmfree 统一回收。 */
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
    for (uint64 va = 0; va < sz; va += PGSIZE) {
        pte_t *pte = walk(old, va, 0);
        if (!pte || (*pte & PTE_V) == 0)
            continue;                       /* 保护页空洞:跳过 */
        uint64 pa = PTE2PA(*pte);
        int flags = (int)PTE_FLAGS(*pte);   /* 权限原样保留 */
        char *mem = (char *)kalloc();
        if (!mem)
            return -1;
        for (uint64 i = 0; i < PGSIZE; i++) /* 经内核恒等映射读父页 */
            mem[i] = ((char *)pa)[i];
        if (mappages(new, va, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            return -1;
        }
    }
    return 0;
}

/* uvmfree:释放整个用户地址空间
 * [0,sz) 数据页回收;TRAPFRAME 物理页归内核(tf_pool)、TRAMPOLINE 页共享,
 * 仅拆 PTE 不释放;中间页表页由 prune 回收;最后释放根页表页。 */
void
uvmfree(pagetable_t pt, uint64 sz)
{
    if (!pt)
        return;
    unmappings(pt, 0, sz, 1);
    unmappings(pt, TRAPFRAME, PGSIZE, 0);
    unmappings(pt, TRAMPOLINE, PGSIZE, 0);
    kfree((void *)pt);
}

/* ============ lab3 阶段六 · sbrk EAGER 增长/收缩 ============ */

/* uvmalloc:把用户地址空间从 oldsz 增长到 newsz(EAGER 语义)
 * 为 [PGROUNDUP(oldsz), PGROUNDUP(newsz)) 逐页 kalloc 并建立 RWU 映射。
 * 失败(配额耗尽/中间页分配失败/越过 TRAPFRAME)返回 -1,并完整回滚:
 * 本次已建映射的数据页经 unmappings 回收、变空的中间页由 prune 回收,
 * p->sz 由调用方保持不变,进程可继续运行。
 * VA 上界:堆顶不得达到 TRAPFRAME(该页是内核所有的陷入现场页,
 * 用户页表在此处只有 RW 无 U;继续增长会 remap panic)。 */
int
uvmalloc(pagetable_t pt, uint64 oldsz, uint64 newsz)
{
    if (newsz < oldsz)                      /* 回绕:非法增长请求 */
        return -1;
    if (newsz == oldsz)
        return 0;
    if (PGROUNDUP(newsz) > TRAPFRAME)       /* 越过内核固定映射区 */
        return -1;

    uint64 start = PGROUNDUP(oldsz);
    for (uint64 va = start; va < newsz; va += PGSIZE) {
        char *mem = (char *)kalloc();
        if (!mem)
            goto fail;
        if (mappages(pt, va, PGSIZE, (uint64)mem,
                     PTE_R | PTE_W | PTE_U | PTE_A | PTE_D) != 0) {
            kfree(mem);
            goto fail;
        }
    }
    return 0;

fail:
    /* 空洞跳过语义:可安全回收 [start, newsz) 内本次已建的部分 */
    unmappings(pt, start, newsz - start, 1);
    return -1;
}

/* uvmdealloc:把用户地址空间从 oldsz 收缩到 newsz
 * 只拆除整页 [PGROUNDUP(newsz), oldsz),回收数据物理页;
 * newsz 所在页内保留(其低地址部分仍属用户空间)。
 * 调用方保证 newsz < oldsz(本函数不再重复校验)。 */
int
uvmdealloc(pagetable_t pt, uint64 oldsz, uint64 newsz)
{
    uint64 start = PGROUNDUP(newsz);
    if (start < oldsz)
        unmappings(pt, start, oldsz - start, 1);
    return 0;
}

/* va_ok:用户地址区间 [va, va+len) 合法(不越过 MAXVA、不溢出)
 * walk 本身只按 9+9+9 位取索引,越界 VA 会落到高地址页表项上,
 * 必须在入口显式拒绝,避免把内核结构/未定义区域当成用户页访问。 */
static int
va_ok(uint64 va, uint64 len)
{
    if (va >= MAXVA)
        return 0;
    if (len > 0 && va + len - 1 < va)      /* 加法回绕 */
        return 0;
    if (len > 0 && va + len - 1 >= MAXVA)
        return 0;
    return 1;
}

/* copyin:从用户地址 srcva 拷 len 字节到内核 dst(跨页逐页翻译)
 * 页不存在或无 U 位返回 -1。经 PTE 得物理地址后走内核恒等映射,
 * 不需要 SSTATUS_SUM。 */
int
copyin(pagetable_t pt, char *dst, uint64 srcva, uint64 len)
{
    if (!va_ok(srcva, len))
        return -1;
    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(srcva);
        pte_t *pte = walk(pt, va0, 0);
        if (!pte || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
            return -1;
        uint64 n = PGSIZE - (srcva - va0);
        if (n > len) n = len;
        char *pa = (char *)(PTE2PA(*pte) + (srcva - va0));
        for (uint64 i = 0; i < n; i++)
            dst[i] = pa[i];
        dst += n; srcva += n; len -= n;
    }
    return 0;
}

/* copyout:从内核 src 拷 len 字节到用户地址 dstva
 * 额外要求目标页有 W 位(代码页 RX,copyout 失败返回 -1)。 */
int
copyout(pagetable_t pt, uint64 dstva, char *src, uint64 len)
{
    if (!va_ok(dstva, len))
        return -1;
    while (len > 0) {
        uint64 va0 = PGROUNDDOWN(dstva);
        pte_t *pte = walk(pt, va0, 0);
        if (!pte || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
            (*pte & PTE_W) == 0)
            return -1;
        uint64 n = PGSIZE - (dstva - va0);
        if (n > len) n = len;
        char *pa = (char *)(PTE2PA(*pte) + (dstva - va0));
        for (uint64 i = 0; i < n; i++)
            pa[i] = src[i];
        src += n; dstva += n; len -= n;
    }
    return 0;
}

/* copyinstr:从用户地址 srcva 拷 NUL 结尾字符串到内核 dst(至多 max 字节,
 * 含 NUL)。成功(遇到 NUL)返回 0;越界/超长/无映射返回 -1。 */
int
copyinstr(pagetable_t pt, char *dst, uint64 srcva, uint64 max)
{
    if (!va_ok(srcva, max))
        return -1;
    while (max > 0) {
        uint64 va0 = PGROUNDDOWN(srcva);
        pte_t *pte = walk(pt, va0, 0);
        if (!pte || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
            return -1;
        uint64 n = PGSIZE - (srcva - va0);
        if (n > max) n = max;
        char *pa = (char *)(PTE2PA(*pte) + (srcva - va0));
        for (uint64 i = 0; i < n; i++) {
            char c = pa[i];
            dst[i] = c;
            if (c == '\0')
                return 0;
        }
        dst += n; srcva += n; max -= n;
    }
    return -1;                              /* max 内未见 NUL */
}

/* ============ lab3 收尾 · 页表观测 dump_pagetable ============
 * 输出格式严格遵循 gifts/dump_pagetable-ABI.md:
 *   DUMP-PAGETABLE begin
 *   L2   va=0x..0 pa=0x..0 perm=----      (非叶表项)
 *   L1   va=0x..0 pa=0x..0 perm=----
 *   LEAF va=0x..0 pa=0x..0 perm=rw-u      (叶子页表项)
 *   DUMP-PAGETABLE end
 * perm 依次按 R/W/X/U 四位输出,未置位用 '-' 占位;
 * A/D 属硬件运行时状态位,不进入 perm(验收时口头说明)。
 * va 为该表项管辖区间的起始地址;逐级按 index 递增遍历,输出天然升序。
 * 函数不绑定单一进程:调用方传入任意 pagetable 均可重放。 */

/* dump_perm:把 PTE 权限位转成 "rwxu" 四位字符串(未置位为 '-') */
static void
dump_perm(int flags, char *buf)
{
    buf[0] = (flags & PTE_R) ? 'r' : '-';
    buf[1] = (flags & PTE_W) ? 'w' : '-';
    buf[2] = (flags & PTE_X) ? 'x' : '-';
    buf[3] = (flags & PTE_U) ? 'u' : '-';
    buf[4] = '\0';
}

/* dump_level:递归输出某级页表
 *   level=2 根表、1 L1 表、0 L0 表;
 *   base 为本级第 0 项管辖区间的起始 VA。
 * 叶子判定:R/W/X 任一置位即为叶子(含 L1 2MB / L2 1GB 超级页)。 */
static void
dump_level(pagetable_t pt, int level, uint64 base)
{
    for (int i = 0; i < 512; i++) {
        uint64 va = base | ((uint64)i << (12 + 9 * level));
        pte_t pte = pt[i];
        if ((pte & PTE_V) == 0)
            continue;

        char perm[5];
        dump_perm((int)PTE_FLAGS(pte), perm);

        if (pte & (PTE_R | PTE_W | PTE_X)) {
            printf("LEAF va=%x pa=%x perm=%s\n", va, PTE2PA(pte), perm);
        } else if (level > 0) {
            printf("L%d va=%x pa=%x perm=----\n", level, va, PTE2PA(pte));
            dump_level((pagetable_t)PTE2PA(pte), level - 1, va);
        } else {
            /* L0 项必须为叶子,V 置位却无 RWX 属非法表项,按叶子原样输出 */
            printf("LEAF va=%x pa=%x perm=%s\n", va, PTE2PA(pte), perm);
        }
    }
}

/* dump_pagetable:输出一个 SV39 页表的全部有效表项 */
void
dump_pagetable(pagetable_t pt)
{
    if (!pt)
        return;
    printf("DUMP-PAGETABLE begin\n");
    dump_level(pt, 2, 0);
    printf("DUMP-PAGETABLE end\n");
}

/* kstack_map:为进程槽 idx 在内核页表 KSTACK(idx) 高地址建内核栈映射
 * (lab0 快照图布局:栈页下方留一页不映射作溢出保护)。
 * 返回栈顶 VA(KSTACK(idx)+PGSIZE);失败返回 0。 */
uint64
kstack_map(int idx)
{
    if (!g_kpgtbl)
        return 0;
    char *mem = (char *)kalloc();
    if (!mem)
        return 0;
    uint64 va = KSTACK(idx);
    if (mappages(g_kpgtbl, va, PGSIZE, (uint64)mem,
                 PTE_R | PTE_W | PTE_A | PTE_D) != 0) {
        kfree(mem);
        return 0;
    }
    sfence_vma();
    return va + PGSIZE;
}

/* kstack_unmap:拆除进程槽 idx 的内核栈映射并回收物理页 */
void
kstack_unmap(int idx)
{
    if (!g_kpgtbl)
        return;
    unmappings(g_kpgtbl, KSTACK(idx), PGSIZE, 1);
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
 *   CLINT 不映射:时钟中断初始化暂延后,先推进阶段 3-4。
 * - TRAMPOLINE 映射在最高地址:跳板页必须与用户页表同址,
 *   uservec/userret 切 satp 前后指令流才能不断档。
 * - TRAPFRAME 不进内核页表:内核经 p->trapframe 的内核 VA(bss 恒等映射)
 *   直接访问;TRAPFRAME 固定 VA 的映射由每个用户页表各自携带(allocproc)。
 *
 * sfence.vma 时机:本函数只建表不刷 TLB;刷 TLB 由各切换点负责——
 *   kvminithart 写 satp 后、uservec/userret 换页表前后(trampoline
 *   预置)、kstack_map/kstack_unmap 改 PTE 后。批量建映射由调用方统一刷。
 * 任一步失败说明早期内存不足,直接 panic。 */
pagetable_t
kvminit(uint64 trampoline_pa)
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

    /* CLINT 不映射:时钟中断暂延后 */

    /* TRAMPOLINE 需要单独映射(在高地址,不在 superpage 范围内) */
    if (mappages(kpgtbl, TRAMPOLINE, PGSIZE, trampoline_pa, PTE_R | PTE_X | PTE_A | PTE_D) != 0)
        vm_panic("kvminit: map TRAMPOLINE");

    /* 保存全局引用:kstack_map 改内核页表、uvmcreate 取跳板页地址 */
    g_kpgtbl = kpgtbl;
    g_trampoline_pa = trampoline_pa;

    return kpgtbl;
}

/* kvminithart:把 satp 切到内核页表 */
void
kvminithart(pagetable_t kpgtbl)
{
    w_satp(MAKE_SATP(kpgtbl));
    sfence_vma();
}
