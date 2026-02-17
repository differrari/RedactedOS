#kernel
# toolchain (inherited from top-level)

include ../common.mk

CPPFLAGS := -I. -I../shared -I../user -DXHCI_CTX_SIZE=$(XHCI_CTX_SIZE)

MODULES := $(wildcard ../modules/*.a)

ifeq ($(QEMU),true)
  CPPFLAGS += -DQEMU
endif

ifeq ($(TEST),true)
  CPPFLAGS += -DTEST
endif

CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)
LDFLAGS  := $(LDFLAGS_BASE) -T $(shell ls *.ld) --defsym=LOAD_ADDR=$(LOAD_ADDR)

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
ASM_SRC := $(shell find . -name '*.S')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:%.c=$(BUILD_DIR)/%.o) $(ASM_SRC:%.S=$(BUILD_DIR)/%.o) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.o)
DEP     := $(C_SRC:%.c=$(BUILD_DIR)/%.d) $(ASM_SRC:%.S=$(BUILD_DIR)/%.d) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.d)

OBJL    := $(filter-out $(BUILD_DIR)/./boot.o,$(OBJ))

ELF     := ../kernel.elf
TARGET  := ../kernel.img

all: $(TARGET)

prepare:
	mkdir -p $(BUILD_DIR)

$(TARGET): ../shared/klibshared.a prepare $(OBJ)
	echo $(LIBMODULES)
	$(VLD) $(LDFLAGS) -o $(ELF) $(OBJL) ../shared/klibshared.a $(MODULES) $(BUILD_DIR)/boot.o
	$(OBJCOPY) -O binary $(ELF) $@
	mkdir -p ../fs
	cp $(ELF) ../fs/kernel.elf

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(VAS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(VCC) $(CFLAGS) -c -MMD -MP $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(VCXX) $(CXXFLAGS) -c -MMD -MP $< -o $@

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(ELF) $(TARGET)
	$(RM) -r $(BUILD_DIR)

-include $(DEP)
