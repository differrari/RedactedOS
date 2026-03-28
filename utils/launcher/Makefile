include ../../common.mk

CPPFLAGS := -I. -I../../shared
CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)
LDFLAGS  := -T $(shell ls *.ld)

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:%.c=$(BUILD_DIR)/%.o) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.o)
DEP     := $(C_SRC:%.c=$(BUILD_DIR)/%.d) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.d)

NAME     := $(notdir $(CURDIR))
ELF      := $(NAME).elf
TARGET   := $(NAME).bin
PACKAGE  := $(NAME).red
LOCATION := ../../fs/redos/system/


.PHONY: prepare all clean

all: prepare $(PACKAGE)/$(TARGET)

prepare:
	mkdir -p resources
	mkdir -p $(BUILD_DIR)
	mkdir -p $(PACKAGE)
	mkdir -p $(LOCATION)
	cp -r resources $(PACKAGE)

$(PACKAGE)/$(TARGET): ../../shared/libshared.a $(OBJ)
	$(VLD) $(LDFLAGS) -o $(PACKAGE)/$(ELF) $(addprefix $(BUILD_DIR)/,$(notdir $(OBJ))) ../../shared/libshared.a
	$(OBJCOPY) -O binary $(PACKAGE)/$(ELF) $@
	cp -r $(PACKAGE) $(LOCATION)

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
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(TARGET) 
	$(RM) -r $(PACKAGE) $(BUILD_DIR)
	
dump: all
	$(ARCH)objdump -S $(NAME).red/$(NAME).elf > dump

-include $(DEP)
