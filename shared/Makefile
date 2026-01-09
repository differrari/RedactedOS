#shared

include ../common.mk

CPPFLAGS := -I.
CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)

CLEAN_OBJS := $(shell find . -name "*.o")
CLEAN_DEPS := $(shell find . -name "*.d")
C_SRC   := $(shell find . -name "*.c")
CPP_SRC := $(shell find . -name "*.cpp")
ASM_SRC := $(shell find . -name "*.S")
OBJ     := $(C_SRC:%.c=$(BUILD_DIR)/%.o) $(ASM_SRC:%.S=$(BUILD_DIR)/%.o) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.o)
DEP     := $(C_SRC:%.c=$(BUILD_DIR)/%.d) $(ASM_SRC:%.S=$(BUILD_DIR)/%.d) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.d)

TARGET  := libshared.a

.PHONY: all clean prepare

all: prepare $(TARGET)

prepare:
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJ)
	@echo "Finishing build $(ARCH)"
	echo $(addprefix $(BUILD_DIR)/,$(notdir $(OBJ)))
	$(VAR) rcs $@ $(OBJ)

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(VAS) $(CFLAGS) $(SH_FLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(VCC) $(CFLAGS) $(SH_FLAGS) -c -MMD -MP $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(VCXX) $(CXXFLAGS) $(SH_FLAGS) -c -MMD -MP $< -o $@

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(TARGET)
	$(RM) -r $(BUILD_DIR)

-include $(DEP)
