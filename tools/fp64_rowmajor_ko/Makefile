# SPDX-License-Identifier: GPL-2.0
# fp64_rowmajor_ldp_stp — 外部内核模块构建
#
# 模块源文件使用 NEON 内建函数 (fp64 fadd/fsub/fmul/fmax) 与内联汇编,
# 而 arm64 内核全局以 -mgeneral-regs-only 编译。参照内核自身做法
# (lib/Makefile 对 xor-neon.o 的处理), 对本编译单元移除该标志。

obj-m += fp64_rowmajor_ldp_stp.o

# 与内核 arch/arm64/lib/Makefile 对 xor-neon.o 的处理完全一致:
# 1) 移除全局 -mgeneral-regs-only 以允许 NEON 内建函数;
# 2) -ffreestanding + -isystem gcc 内部 include, 使 arm_neon.h 可用。
CFLAGS_REMOVE_fp64_rowmajor_ldp_stp.o := -mgeneral-regs-only
CFLAGS_fp64_rowmajor_ldp_stp.o += -ffreestanding
CFLAGS_fp64_rowmajor_ldp_stp.o += -isystem $(shell $(CC) -print-file-name=include)

KDIR ?= /lib/modules/$(shell uname -r)/build
# 本模块仅面向 arm64; 显式指定避免环境变量 ARCH=aarch64 污染 kbuild
KARCH := arm64

.PHONY: all ref clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(KARCH) modules

ref:
	gcc -O2 -Wall -Wextra -o userspace_ref userspace_ref.c

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(KARCH) clean
	rm -f userspace_ref ref.out ref.err
