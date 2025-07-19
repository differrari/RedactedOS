MODE ?= virt
LOAD_ADDR ?= 0x41000000
XHCI_CTX_SIZE ?= 32

OS := $(shell uname)

ifeq ($(OS),Darwin)
BOOTFS=/Volumes/bootfs
else
BOOTFS=/media/bootfs
endif

.PHONY: all kernel user shared clean raspi virt run debug dump

all: shared user kernel
	@echo "Build complete."
	./createfs

dump:
	aarch64-none-elf-objdump -D kernel.elf > dump

shared:
	$(MAKE) -C shared

user:
	$(MAKE) -C user

kernel:
	$(MAKE) -C kernel LOAD_ADDR=$(LOAD_ADDR) XHCI_CTX_SIZE=$(XHCI_CTX_SIZE)

clean:
	$(MAKE) -C shared clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean

raspi:
	$(MAKE) LOAD_ADDR=0x80000 XHCI_CTX_SIZE=64 all

virt:
	$(MAKE) LOAD_ADDR=0x41000000 XHCI_CTX_SIZE=32 all

debug:
	$(MAKE) $(MODE)
	./rundebug MODE=$(MODE) $(ARGS)

install:
	$(MAKE) clean
	$(MAKE) LOAD_ADDR=0x80000 all
	cp kernel.img $(BOOTFS)/kernel8.img
	cp kernel.img $(BOOTFS)/kernel_2712.img

run: 
	$(MAKE) $(MODE)
	./run_$(MODE)