.PHONY: all kernel user shared clean

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

debugrpi:
	$(MAKE) rpi
	./rundebug_r

debugvirtio:
	$(MAKE) virtio
	./rundebug_v

run: 
	$(MAKE) rpi
	./run_rpi