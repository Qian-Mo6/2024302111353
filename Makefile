# lab1 骨架 Makefile + lab2 升级规则(合并)
CC   = riscv64-unknown-elf-gcc
LD   = riscv64-unknown-elf-ld
CFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc -fno-stack-protector -fno-pie
QEMU = qemu-system-riscv64

OBJCOPY ?= riscv64-unknown-elf-objcopy

OBJS = kernel/entry.o kernel/start.o kernel/console.o kernel/printf.o kernel/main.o \
       kernel/proc.o kernel/trap.o kernel/syscall.o kernel/vm.o kernel/kalloc.o kernel/trampoline.o kernel/kernelvec.o kernel/userimg.o kernel/plic.o

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

UFLAGS = -Wall -Werror -O -std=gnu99 -mcmodel=medany -ffreestanding \
         -nostdlib -fno-common -ggdb -march=rv64gc \
         -fno-stack-protector -fno-pie -I.

# usys 桩由 perl 脚本生成(课程赠送,勿改)
user/usys.S: user/usys.pl
	perl user/usys.pl > user/usys.S

# 用户程序 → 链接(基址 0)→ 平铺二进制
user-flat/%.bin: user/%.c user/user.h user/ulib.c user/printf.c user/usys.S
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
	    user-flat/$*.o $$EXTRA_OBJS user-flat/ulib.o user-flat/printf.o user-flat/usys.o
	$(OBJCOPY) -O binary user-flat/$*.elf $@

# 内嵌程序表(ABI,勿改):
#   _uprog_table: 每项 = .quad start; .quad end; .asciz "name";
#   以 .quad 0; .quad 0 结尾。内核侧按表遍历。
kernel/userimg.S: Makefile $(addprefix user-flat/,$(UPROGS:=.bin))
	printf '.section .rodata\n .global _uprog_table\n_uprog_table:\n' > $@.tmp
	for p in $(UPROGS); do \
	  printf ' .quad _uprog_%s_start\n .quad _uprog_%s_end\n .asciz "%s"\n .balign 8\n' \
	    $$p $$p $$p >> $@.tmp; \
	  printf ' .global _uprog_%s_start\n_uprog_%s_start:\n .incbin "user-flat/%s.bin"\n .global _uprog_%s_end\n_uprog_%s_end:\n .balign 8\n' \
	    $$p $$p $$p $$p $$p >> $@.tmp; \
	done
	printf ' .quad 0\n .quad 0\n' >> $@.tmp
	mv $@.tmp $@

clean-extra:
	rm -rf user-flat kernel/userimg.S user/usys.S
