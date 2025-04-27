.PHONY: all kernel shared clean

all: shared/libshared.a kernel
	@echo "Build complete."

shared/libshared.a:
	$(MAKE) -C shared

kernel:
	$(MAKE) -C kernel

clean:
	$(MAKE) -C shared clean
	$(MAKE) -C kernel clean