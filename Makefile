MODE ?= rpi

.PHONY: all kernel user shared clean rpi virtio debugrpi debugvirtio run debug

all: shared user kernel
	@echo "Build complete."
	./createfs

shared:
	$(MAKE) -C shared

user:
	$(MAKE) -C user

kernel:
	$(MAKE) -C kernel LOAD_ADDR=$(LOAD_ADDR)

clean:
	$(MAKE) -C shared clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean

rpi:
	$(MAKE) LOAD_ADDR=0x80000 all

virtio:
	$(MAKE) LOAD_ADDR=0x41000000 all

debug:
	$(MAKE) $(MODE)
	./rundebug MODE=$(MODE) $(ARGS)

run: 
	$(MAKE) $(MODE)
	./run_$(MODE)