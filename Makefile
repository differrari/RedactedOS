.PHONY: all kernel user shared clean

#user/libuser.a
all: shared/libshared.a kernel
	@echo "Build complete."

shared/libshared.a:
	$(MAKE) -C shared

user/libuser.a:
	$(MAKE) -C user

kernel:
	$(MAKE) -C kernel

clean:
	$(MAKE) -C shared clean
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean