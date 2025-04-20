.PHONY: all kernel clean

all: kernel
	@echo "Build complete."

# shared:
# 	$(MAKE) -C shared

kernel:
	$(MAKE) -C kernel

#user:
#    $(MAKE) -C user

clean:
#	$(MAKE) -C shared clean
	$(MAKE) -C kernel clean
#	#$(MAKE) -C user clean
