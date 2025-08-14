include common.mk

OS      := $(shell uname)
FS_DIRS := fs/redos/user

ifeq ($(OS),Darwin)
BOOTFS := /Volumes/bootfs
else
BOOTFS := /media/bootfs
endif

.PHONY: all shared user kernel clean raspi virt run debug dump prepare-fs help install

all: shared kernel user 
	@echo "Build complete."
	./createfs

shared:
	$(MAKE) -C shared

user: shared prepare-fs
	$(MAKE) -C user

kernel: shared
	$(MAKE) -C kernel LOAD_ADDR=$(LOAD_ADDR) XHCI_CTX_SIZE=$(XHCI_CTX_SIZE) QEMU=$(QEMU)

clean:
	$(MAKE) -C shared $@
	$(MAKE) -C user   $@
	$(MAKE) -C kernel $@
	@echo "removing fs dirs"
	$(RM) -r $(FS_DIRS)
	@echo "removing images"
	$(RM) kernel.img kernel.elf disk.img dump

raspi:
	$(MAKE) LOAD_ADDR=0x80000 XHCI_CTX_SIZE=64 QEMU=true all

virt:
	$(MAKE) LOAD_ADDR=0x41000000 XHCI_CTX_SIZE=32 QEMU=true all

run:
	$(MAKE) $(MODE)
	./run_$(MODE)

debug:
	$(MAKE) $(MODE)
	./rundebug MODE=$(MODE) $(ARGS)

dump:
	$(OBJCOPY) -O binary kernel.elf kernel.img
	$(ARCH)-objdump -D kernel.elf > dump

install:
	$(MAKE) clean
	$(MAKE) LOAD_ADDR=0x80000 XHCI_CTX_SIZE=64 QEMU=false all
	cp kernel.img $(BOOTFS)/kernel8.img
	cp kernel.img $(BOOTFS)/kernel_2712.img
	cp config.txt $(BOOTFS)/config.txt

prepare-fs:
	@echo "creating dirs"
	@mkdir -p $(FS_DIRS)

help:
	@printf "usage:\n\
  make all          build the os\n\
  make clean        remove all build artifacts\n\
  make raspi        build for raspberry\n\
  make virt         build for qemu virt board\n\
  make run          build and run in virt mode\n\
  make debug        build and run with debugger\n\
  make dump         disassemble kernel.elf\n\
  make install      create raspi kernel and mount it on a bootable partition\n\
  make prepare-fs   create directories for the filesystem\n\n"\
  \n\
  Use 'make V=1' for verbose build output.
