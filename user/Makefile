#user

include ../common.mk

CPPFLAGS := -I. -I../shared
CFLAGS   := $(CFLAGS_BASE) $(CPPFLAGS)
CXXFLAGS := $(CXXFLAGS_BASE) $(CPPFLAGS)
LDFLAGS  := -T $(shell ls *.ld)

CLEAN_OBJS := $(shell find . -name '*.o')
CLEAN_DEPS := $(shell find . -name '*.d')
C_SRC   := $(shell find . -name '*.c')
CPP_SRC := $(shell find . -name '*.cpp')
OBJ     := $(C_SRC:.c=.o) $(CPP_SRC:.cpp=.o)
DEP     := $(C_SRC:.c=.d) $(CPP_SRC:.cpp=.d)

NAME     := $(notdir $(CURDIR))
ELF      := $(NAME).elf
TARGET   := $(NAME).bin
LOCATION := ../fs/redos/user/

.PHONY: prepare all clean

all: prepare $(LOCATION)$(TARGET)

prepare:
	mkdir -p $(BUILD_DIR)

$(LOCATION)$(TARGET): ../shared/libshared.a $(OBJ)
	echo $(LDFLAGS) -o $(LOCATION)$(ELF) $(addprefix $(BUILD_DIR)/,$(notdir $(OBJ))) ../shared/libshared.a
	$(VLD) $(LDFLAGS) -o $(LOCATION)$(ELF) $(addprefix $(BUILD_DIR)/,$(notdir $(OBJ))) ../shared/libshared.a
	$(OBJCOPY) -O binary $(LOCATION)$(ELF) $@

%.o: %.S
	$(VAS) $(CFLAGS) -c $< -o $(BUILD_DIR)/$(notdir $@)

%.o: %.c
	$(VCC) $(CFLAGS) -c -MMD -MP $< -o $(BUILD_DIR)/$(notdir $@)

%.o: %.cpp
	$(VCXX) $(CXXFLAGS) -c -MMD -MP $< -o $(BUILD_DIR)/$(notdir $@)

clean:
	$(RM) $(CLEAN_OBJS) $(CLEAN_DEPS) $(TARGET)

-include $(DEP)
