include common.mk

OS      := $(shell uname)
FS_DIRS := fs/redos/user

ifeq ($(OS),Darwin)
BOOTFS := /Volumes/bootfs
else
BOOTFS := /media/bootfs
endif

.PHONY: all shared user kernel clean raspi virt run debug dump prepare-fs help install

all: kshared kernel shared user utils bins
	@echo "Build complete."
	./createfs

kshared:
	$(MAKE) -C shared SH_FLAGS=-DKERNEL BUILD_DIR=./kbuild TARGET=klibshared.a

shared: 
	$(MAKE) -C shared BUILD_DIR=./build

user: shared prepare-fs
	$(MAKE) -C user

kernel: kshared
	$(MAKE) -C kernel LOAD_ADDR=$(LOAD_ADDR) XHCI_CTX_SIZE=$(XHCI_CTX_SIZE) QEMU=$(QEMU)

utils: shared prepare-fs
	$(MAKE) -C utils

bins: shared prepare-fs
	$(MAKE) -C bin

clean:
	$(MAKE) -C shared $@
	$(MAKE) -C user   $@
	$(MAKE) -C kernel $@
	$(MAKE) -C utils  $@
	$(MAKE) -C bin  $@
	@echo "removing fs dirs"
	$(RM) -r $(FS_DIRS)
	@echo "removing images"
	$(RM) kernel.img kernel.elf disk.img dump

raspi:
	$(MAKE) LOAD_ADDR=0x80000 XHCI_CTX_SIZE=64 QEMU=true all
	./run_raspi

virt:
	$(MAKE) LOAD_ADDR=0x41000000 XHCI_CTX_SIZE=32 QEMU=true all

run:
	$(MAKE) $(MODE)
	./run_$(MODE)

debug:
	$(MAKE) $(MODE)
	./rundebug MODE=$(MODE) $(ARGS)

dump:
	$(ARCH)-objdump -D kernel.elf > dump
	$(MAKE) -C user $@

install:
	$(MAKE) clean
	$(MAKE) LOAD_ADDR=0x80000 XHCI_CTX_SIZE=64 QEMU=false all
	cp kernel.img $(BOOTFS)/kernel8.img
	cp kernel.img $(BOOTFS)/kernel_2712.img
	cp config.txt $(BOOTFS)/config.txt
	cp kernel.elf $(BOOTFS)/kernel.elf

prepare-fs:
	@echo "creating dirs"
	@mkdir -p $(FS_DIRS)

help:
	@printf "usage:\n\
  make all          build the os\n\
  make clean        remove all build artifacts\n\
  make raspi        build for raspberry pi\n\
  make virt         build for qemu virt board\n\
  make run          build and run in virt mode\n\
  make debug        build and run with debugger\n\
  make dump         disassemble kernel.elf\n\
  make install      create raspi kernel and mount it on a bootable partition\n\
  make prepare-fs   create directories for the filesystem\n\n"\
  \n\
  Use 'make V=1' for verbose build output.
