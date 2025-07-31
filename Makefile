ARCH       ?= aarch64-none-elf
CC         := $(ARCH)-gcc
LD         := $(ARCH)-ld
AR         := $(ARCH)-ar
OBJCOPY    := $(ARCH)-objcopy

CFLAGS_BASE  ?= -g -O0 -nostdlib -ffreestanding \
                -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
                -Wall -Wextra -Wno-unused-parameter -mcpu=cortex-a72
CONLY_FLAGS_BASE ?= -std=c17
LDFLAGS_BASE ?=

LOAD_ADDR      ?= 0x41000000
XHCI_CTX_SIZE  ?= 32
QEMU           ?= true
MODE           ?= virt

export ARCH CC LD AR OBJCOPY CFLAGS_BASE CONLY_FLAGS_BASE LDFLAGS_BASE LOAD_ADDR XHCI_CTX_SIZE QEMU

OS      := $(shell uname)
FS_DIRS := fs/redos/user

ifeq ($(OS),Darwin)
BOOTFS := /Volumes/bootfs
else
BOOTFS := /media/bootfs
endif

.PHONY: all shared user kernel clean raspi virt run debug dump prepare-fs help install

all: shared user kernel
	@echo "Build complete."
	./createfs

shared:
	$(MAKE) -C shared

user: prepare-fs
	$(MAKE) -C user

kernel:
	$(MAKE) -C kernel LOAD_ADDR=$(LOAD_ADDR) XHCI_CTX_SIZE=$(XHCI_CTX_SIZE) QEMU=$(QEMU)

clean:
	$(MAKE) -C shared clean
	$(MAKE) -C user   clean
	$(MAKE) -C kernel clean
	@echo "removing fs dirs"
	rm -rf $(FS_DIRS)
	@echo "removing images"
	rm -f kernel.img kernel.elf disk.img dump

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
	aarch64-none-elf-objdump -D kernel.elf > dump
  
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
  make prepare-fs   create directories for the filesystem\n\n"
