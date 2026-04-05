include ../../common.mk

CPPFLAGS := -I. -I../../shared
CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)
LDFLAGS  := -e _start

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:%.c=$(BUILD_DIR)/%.o) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.o)
DEP     := $(CRT0_DEP) $(C_SRC:%.c=$(BUILD_DIR)/%.d) $(CPP_SRC:%.cpp=$(BUILD_DIR)/%.d)
CRT0_SRC := ../../user/crt0.c
CRT0_OBJ := $(BUILD_DIR)/crt0.o
CRT0_DEP := $(BUILD_DIR)/crt0.d

NAME     := $(notdir $(CURDIR))
ELF      := $(NAME).elf
TARGET   := $(NAME).bin
PACKAGE  := $(NAME).red
LOCATION := ../../fs/redos/user/


.PHONY: prepare all clean

all: prepare $(PACKAGE)/$(TARGET)

prepare:
	mkdir -p resources
	mkdir -p $(BUILD_DIR)
	mkdir -p $(PACKAGE)
	cp -r resources $(PACKAGE)

$(PACKAGE)/$(TARGET): ../../shared/libshared.a $(CRT0_OBJ) $(OBJ)
	$(VLD) $(LDFLAGS) -o $(PACKAGE)/$(ELF) $(CRT0_OBJ) $(addprefix $(BUILD_DIR)/,$(notdir $(OBJ))) ../../shared/libshared.a
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


$(CRT0_OBJ): $(CRT0_SRC)
	@mkdir -p $(dir $@)
	$(VCC) $(CFLAGS) -c -MMD -MP $< -o $@

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(TARGET) 
	$(RM) -r $(PACKAGE) $(BUILD_DIR)
	
dump: all
	$(ARCH)objdump -S $(NAME).red/$(NAME).elf > dump

-include $(DEP)
