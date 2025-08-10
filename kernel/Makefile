#kernel
# toolchain (inherited from top-level)

CPPFLAGS := -I. -I../shared -I../user -DXHCI_CTX_SIZE=$(XHCI_CTX_SIZE)

ifeq ($(QEMU),true)
  CPPFLAGS += -DQEMU
endif

CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)
LDFLAGS  := $(LDFLAGS_BASE) -T $(shell ls *.ld) --defsym=LOAD_ADDR=$(LOAD_ADDR)

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
ASM_SRC := $(shell find . -name '*.S')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:.c=.o) $(ASM_SRC:.S=.o) $(CPP_SRC:.cpp=.o)
OBJL    := $(filter-out ./boot.o,$(OBJ))
DEP     := $(C_SRC:.c=.d) $(ASM_SRC:.S=.d) $(CPP_SRC:.cpp=.d)

ELF     := ../kernel.elf
TARGET  := ../kernel.img

all: $(TARGET)

$(TARGET): ../shared/libshared.a $(OBJ)
	$(LD) $(LDFLAGS) -o $(ELF) $(OBJL) ../shared/libshared.a
	$(OBJCOPY) -O binary $(ELF) $@

%.o: %.S
	$(CC) $(CFLAGS) -c -MMD -MP $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c -MMD -MP $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -MMD -MP $< -o $@

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(ELF) $(TARGET)

-include $(DEP)
