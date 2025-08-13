#kernel
# toolchain (inherited from top-level)

include ../common.mk

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
OBJL    := $(filter-out $(BUILD_DIR)/boot.o,$(addprefix $(BUILD_DIR)/,$(notdir $(OBJ))))
DEP     := $(C_SRC:.c=.d) $(ASM_SRC:.S=.d) $(CPP_SRC:.cpp=.d)

ELF     := ../kernel.elf
TARGET  := ../kernel.img

all: $(TARGET)

prepare:
	mkdir -p $(BUILD_DIR)

$(TARGET): ../shared/libshared.a prepare $(OBJ)
	echo $(LDFLAGS)
	$(VLD) $(LDFLAGS) -o $(ELF) $(OBJL) ../shared/libshared.a $(BUILD_DIR)/boot.o
	$(OBJCOPY) -O binary $(ELF) $@

%.o: %.S
	$(VAS) $(CFLAGS) -c $< -o $(BUILD_DIR)/$(notdir $@)

%.o: %.c
	$(VCC) $(CFLAGS) -c -MMD -MP $< -o $(BUILD_DIR)/$(notdir $@)

%.o: %.cpp
	$(VCXX) $(CXXFLAGS) -c -MMD -MP $< -o $(BUILD_DIR)/$(notdir $@)

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(ELF) $(TARGET)

-include $(DEP)
