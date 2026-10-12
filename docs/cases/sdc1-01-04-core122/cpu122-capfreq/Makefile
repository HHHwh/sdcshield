# cpu122_capfreq — 依赖目标内核的 kernel-devel（板上: 6.6.0-159.4.13.167）
# 内核升级后必须在此重编（SOP 见 README.md），脚本门0 会拦截 vermagic 不匹配的失载。
obj-m += cpu122_capfreq.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
