#kernel
# toolchain (inherited from top-level)

CFLAGS := $(CFLAGS_BASE) -I. -I../shared -I../user -DXHCI_CTX_SIZE=$(XHCI_CTX_SIZE)
ifeq ($(QEMU),true)
  CFLAGS += -DQEMU
endif

LDFLAGS := $(LDFLAGS_BASE) -T $(shell ls *.ld) --defsym=LOAD_ADDR=$(LOAD_ADDR)

C_SRC   := $(shell find . -name '*.c')
ASM_SRC := $(shell find . -name '*.S')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:.c=.o) $(ASM_SRC:.S=.o) $(CPP_SRC:.cpp=.o)
OBJL    := $(filter-out ./boot.o,$(OBJ))

ELF     := kernel.elf
TARGET  := kernel.img

all: $(TARGET)

$(TARGET): ../shared/libshared.a $(OBJ)
	$(LD) $(LDFLAGS) -o $(ELF) $(OBJL) ../shared/libshared.a
	$(OBJCOPY) -O binary $(ELF) $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
%.o: %.cpp
	$(CC) $(CFLAGS) -fno-rtti -c $< -o $@

clean:
	rm -f $(OBJ) $(ELF) $(TARGET)