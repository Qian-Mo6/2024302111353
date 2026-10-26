# lab1 骨架 Makefile + lab2 升级规则(合并)
CC   = riscv64-unknown-elf-gcc
LD   = riscv64-unknown-elf-ld
CFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc -fno-stack-protector -fno-pie
QEMU = qemu-system-riscv64

OBJCOPY ?= riscv64-unknown-elf-objcopy
OBJDUMP ?= riscv64-unknown-elf-objdump
NM      ?= riscv64-unknown-elf-nm

OBJS = kernel/entry.o kernel/start.o kernel/console.o kernel/printf.o kernel/main.o \
       kernel/proc.o kernel/trap.o kernel/syscall.o kernel/vm.o kernel/kalloc.o kernel/swtch.o kernel/trampoline.o kernel/kernelvec.o kernel/userimg.o kernel/plic.o

kernel/kernel: $(OBJS) kernel/kernel.ld
	$(LD) -T kernel/kernel.ld -o $@ $(OBJS)

%.o: %.c kernel/riscv.h kernel/course_sid.h
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -Ikernel -c -o $@ $<

# 验收环境固定(排雷环节禁止改动本行以下内容)
qemu: kernel/kernel
	$(QEMU) -machine virt -bios none -kernel kernel/kernel -nographic

clean:
	rm -f kernel/*.o kernel/kernel

# ============ lab2 升级规则 ============
# lab3: 追加官方测试 badaccess(越界终止) 与 memcap(内存上限/OOM)
UPROGS = sh hi spin badecall bufstorm badaccess memcap
UPROGS += bench lottery_test stride_test

UFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc \
         -fno-stack-protector -fno-pie -I.

# usys 桩由 perl 脚本生成(课程赠送,勿改)
user/usys.S: user/usys.pl
	perl user/usys.pl > user/usys.S

# pagepad.o:把 .data 输出段对齐到页边界(lab3 段权限前提)
# 小程序的代码/只读与 .data/.bss 原本同处一页,页粒度下代码页无法去 W;
# 链接时把本目标置于最前,其 .data 输入段带 4096 对齐要求,
# ld 会把整个 .data 输出段对齐到页边界(对齐隙在 .bin 中补零,bss 由内核清零)。
# user.ld 保持不变;程序仍从基址 0 链接。
user-flat/pagepad.o: Makefile
	mkdir -p user-flat
	printf '.section .data\n.balign 4096\n.globl __wstart_pad\n__wstart_pad:\n.byte 0\n' > user-flat/pagepad.S
	$(CC) $(UFLAGS) -Iuser -c user-flat/pagepad.S -o $@

# 用户程序 → 链接(基址 0)→ 平铺二进制
user-flat/%.bin: user/%.c user/user.h user/ulib.c user/printf.c user/usys.S user-flat/pagepad.o
	mkdir -p user-flat
	$(CC) $(UFLAGS) -Iuser -c user/ulib.c   -o user-flat/ulib.o
	$(CC) $(UFLAGS) -Iuser -c user/printf.c -o user-flat/printf.o
	$(CC) $(UFLAGS) -Iuser -c user/usys.S   -o user-flat/usys.o
	$(CC) $(UFLAGS) -Iuser -c $<            -o user-flat/$*.o
	@if [ -f user/common.c ]; then \
	  $(CC) $(UFLAGS) -Iuser -c user/common.c -o user-flat/common.o; \
	  EXTRA_OBJS=user-flat/common.o; \
	else EXTRA_OBJS=; fi; \
	$(LD) -T user/user.ld -o user-flat/$*.elf \
	    user-flat/pagepad.o user-flat/$*.o $$EXTRA_OBJS user-flat/ulib.o user-flat/printf.o user-flat/usys.o
	$(OBJCOPY) -O binary user-flat/$*.elf $@

# 内嵌程序表(ABI,勿改):
#   _uprog_table: 每项 = .quad start; .quad end; .asciz "name";
#   以 .quad 0; .quad 0 结尾。内核侧按表遍历。
# _uprog_meta(lab3 段权限元数据,与表项同序,附在哨兵之后,不改表 ABI):
#   每项 = .quad wstart; .quad memsz;
#   wstart = .data 输出段 VMA(经 pagepad 页对齐),即可写区起始;
#   memsz  = end 符号(含 bss 的镜像总大小)。内核 uvmload 据此区分
#   代码/只读页(RXU,无 W)与数据/bss 页(RWU)。
kernel/userimg.S: Makefile $(addprefix user-flat/,$(UPROGS:=.bin))
	printf '.section .rodata\n .global _uprog_table\n_uprog_table:\n' > $@.tmp
	for p in $(UPROGS); do \
	  printf ' .quad _uprog_%s_start\n .quad _uprog_%s_end\n .asciz "%s"\n .balign 8\n' \
	    $$p $$p $$p >> $@.tmp; \
	  printf ' .global _uprog_%s_start\n_uprog_%s_start:\n .incbin "user-flat/%s.bin"\n .global _uprog_%s_end\n_uprog_%s_end:\n .balign 8\n' \
	    $$p $$p $$p $$p $$p >> $@.tmp; \
	done
	printf ' .quad 0\n .quad 0\n' >> $@.tmp
	printf ' .global _uprog_meta\n_uprog_meta:\n' >> $@.tmp
	for p in $(UPROGS); do \
	  ws=`$(OBJDUMP) -h user-flat/$$p.elf | awk '$$2==".data"{print $$4; exit}'`; \
	  me=`$(NM) user-flat/$$p.elf | awk '$$3=="end"{print $$1; exit}'`; \
	  if [ -z "$$ws" ]; then \
	    ws=`$(OBJDUMP) -h user-flat/$$p.elf | awk '$$2==".bss"{print $$4; exit}'`; \
	  fi; \
	  if [ -z "$$ws" ]; then \
	    ws=`printf '%x' $$(( (0x$$me + 0xfff) & ~0xfff ))`; \
	  fi; \
	  printf ' .quad 0x%s, 0x%s\n' "$$ws" "$$me" >> $@.tmp; \
	done
	mv $@.tmp $@

clean-extra:
	rm -rf user-flat kernel/userimg.S user/usys.S
