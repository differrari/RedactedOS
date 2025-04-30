.PHONY: all kernel user shared clean

all: shared user kernel
	@echo "Build complete."

shared:
	$(MAKE) -C shared

user:
	$(MAKE) -C user

kernel:
	$(MAKE) -C kernel

clean:
	$(MAKE) -C shared clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean