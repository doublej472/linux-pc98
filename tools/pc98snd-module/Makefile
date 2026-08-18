# Build pc98snd.ko against the linux-pc98 kernel tree.
#
#   make            build the module
#   make clean      remove build artifacts
#
# KSRC/KOBJ default to the in-tree kernel source and its O= build dir.

KSRC ?= $(CURDIR)/../../external/kernel/linux-7.1
KOBJ ?= $(CURDIR)/../../build/kernel-7.1-i486

obj-m += pc98snd.o
ccflags-y += -I$(src)/..

all:
	$(MAKE) -C $(KSRC) O=$(KOBJ) M=$(CURDIR) ARCH=i386 modules

clean:
	$(MAKE) -C $(KSRC) O=$(KOBJ) M=$(CURDIR) ARCH=i386 clean

.PHONY: all clean
